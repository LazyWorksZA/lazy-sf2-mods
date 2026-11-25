#include "acmi/acmi_zip.h"

#include <windows.h>

#include <miniz.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lazysf2::acmi {

namespace {

constexpr size_t kChunk = 256 * 1024;

std::atomic<bool> g_cancel{false};      // set by zip_queue_shutdown: running zips abort
std::atomic<uint64_t> g_fail_after{0};  // tests: fail the read once this many bytes were read

struct FileCloser {
    void operator()(FILE* f) const {
        if (f) std::fclose(f);
    }
};
using File = std::unique_ptr<FILE, FileCloser>;

File open_file(const std::wstring& path, const wchar_t* mode) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), mode) != 0) return nullptr;
    return File(f);
}

size_t read_cb(void* opaque, mz_uint64 file_ofs, void* buf, size_t n) {
    FILE* f = static_cast<FILE*>(opaque);
    if (g_cancel.load(std::memory_order_relaxed)) return 0;
    const uint64_t lim = g_fail_after.load(std::memory_order_relaxed);
    if (lim && file_ofs >= lim) return 0;
    if (_fseeki64(f, static_cast<long long>(file_ofs), SEEK_SET) != 0) return 0;
    return std::fread(buf, 1, n, f);
}

std::string narrow_ascii(const std::wstring& w) {
    std::string r;
    for (wchar_t c : w) r += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    return r;
}

bool ends_with(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

/// Size and CRC-32 of a whole file, streamed.
bool scan(FILE* f, uint64_t& size, mz_ulong& crc) {
    std::vector<unsigned char> buf(kChunk);
    if (_fseeki64(f, 0, SEEK_SET) != 0) return false;
    size = 0;
    crc = MZ_CRC32_INIT;
    for (;;) {
        if (g_cancel.load(std::memory_order_relaxed)) return false;
        const size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n) {
            crc = mz_crc32(crc, buf.data(), n);
            size += n;
        }
        if (n < buf.size()) return std::ferror(f) == 0;
    }
}

bool build_zip(const std::wstring& plain, const std::string& entry, const std::wstring& tmp, uint64_t plain_size,
               std::string& error) {
    File in = open_file(plain, L"rb");
    File out = open_file(tmp, L"wb");
    if (!in || !out) {
        error = "could not open the files";
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_cfile(&zip, out.get(), 0)) {
        error = "zip writer init failed";
        return false;
    }
    const mz_bool added = mz_zip_writer_add_read_buf_callback(&zip, entry.c_str(), read_cb, in.get(), plain_size,
                                                              nullptr, nullptr, 0, MZ_DEFAULT_LEVEL, nullptr, 0,
                                                              nullptr, 0);
    const mz_bool done = added && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);  // with init_cfile this does not close the FILE
    if (!done) {
        error = "zip write failed";
        return false;
    }
    if (std::fflush(out.get()) != 0 || std::ferror(out.get())) {
        error = "zip flush failed";
        return false;
    }
    return true;
}

struct CrcSink {
    mz_ulong crc = MZ_CRC32_INIT;
    uint64_t size = 0;
};

size_t crc_cb(void* opaque, mz_uint64, const void* buf, size_t n) {
    if (g_cancel.load(std::memory_order_relaxed)) return 0;
    CrcSink* s = static_cast<CrcSink*>(opaque);
    s->crc = mz_crc32(s->crc, static_cast<const unsigned char*>(buf), n);
    s->size += n;
    return n;
}

/// Reopens the zip and decompresses its entry (streamed), comparing name, size and CRC-32 with the plain file's.
bool verify_zip(const std::wstring& tmp, const std::string& entry, uint64_t plain_size, mz_ulong plain_crc,
                std::string& error) {
    File f = open_file(tmp, L"rb");
    if (!f) {
        error = "could not reopen the zip";
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, f.get(), 0, 0)) {
        error = "the zip does not open";
        return false;
    }
    bool ok = false;
    mz_zip_archive_file_stat st{};
    CrcSink sink;
    if (mz_zip_reader_get_num_files(&zip) != 1 || !mz_zip_reader_file_stat(&zip, 0, &st)) {
        error = "the zip does not hold exactly one entry";
    } else if (entry != st.m_filename || st.m_uncomp_size != plain_size || st.m_crc32 != plain_crc) {
        error = "the zip entry does not match the recording";
    } else if (!mz_zip_reader_extract_to_callback(&zip, 0, crc_cb, &sink, 0)) {
        error = "the zip entry does not decompress";
    } else if (sink.size != plain_size || sink.crc != plain_crc) {
        error = "the decompressed entry differs from the recording";
    } else {
        ok = true;
    }
    mz_zip_reader_end(&zip);
    return ok;
}

}  // namespace

bool compress_acmi(const std::wstring& plain, std::wstring& zip_path, uint64_t& zip_bytes, std::string& error) {
    error.clear();
    if (!ends_with(plain, L".acmi") || ends_with(plain, L".zip.acmi")) {
        error = "not a plain .acmi file";
        return false;
    }
    const size_t slash = plain.find_last_of(L"\\/");
    const std::wstring leaf = plain.substr(slash == std::wstring::npos ? 0 : slash + 1);
    const std::string entry = narrow_ascii(leaf);
    const std::wstring target = plain.substr(0, plain.size() - 5) + L".zip.acmi";
    const std::wstring tmp = target + L".tmp";

    uint64_t plain_size = 0;
    mz_ulong plain_crc = 0;
    {
        File in = open_file(plain, L"rb");
        if (!in || !scan(in.get(), plain_size, plain_crc)) {
            error = "could not read the recording";
            return false;
        }
    }
    bool ok = build_zip(plain, entry, tmp, plain_size, error) && verify_zip(tmp, entry, plain_size, plain_crc, error);
    if (ok && !MoveFileW(tmp.c_str(), target.c_str())) {
        error = "could not rename the zip into place (error " + std::to_string(GetLastError()) + ")";
        ok = false;
    }
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &info))
        zip_bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    zip_path = target;
    if (!DeleteFileW(plain.c_str())) error = "the zip is ready but the plain file could not be deleted";
    return true;
}

namespace {

struct ZipTask {
    std::wstring plain;
    ZipLogFn log;
    ZipDisplayFn display;
};

std::mutex g_lock;
std::deque<ZipTask> g_queue;
std::thread& g_worker = *new std::thread;  // never destroyed: a joinable thread object at exit would terminate
bool g_running = false;  // a worker is processing or about to exit (under g_lock)
bool g_shut = false;     // under g_lock

void worker_main() {
    for (;;) {
        ZipTask t;
        bool cancelled;
        {
            std::lock_guard<std::mutex> g(g_lock);
            if (g_queue.empty()) {
                g_running = false;  // the last act under the lock: a later submit starts a new worker
                return;
            }
            t = std::move(g_queue.front());
            g_queue.pop_front();
            cancelled = g_shut;
        }
        if (cancelled) continue;  // shutting down: leave the plain file alone
        try {
            uint64_t before = 0;
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (GetFileAttributesExW(t.plain.c_str(), GetFileExInfoStandard, &info))
                before = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            std::wstring zip_path;
            uint64_t zip_bytes = 0;
            std::string error;
            if (compress_acmi(t.plain, zip_path, zip_bytes, error)) {
                char line[700];
                std::snprintf(line, sizeof(line), "tacview: packed the recording as %s (%llu bytes, from %llu)",
                              narrow_ascii(t.display ? t.display(zip_path) : zip_path).c_str(),
                              static_cast<unsigned long long>(zip_bytes), static_cast<unsigned long long>(before));
                if (t.log) t.log(line);
                if (!error.empty() && t.log) t.log(("tacview: " + error).c_str());
            } else if (t.log) {
                t.log(("tacview: could not compress the recording (" + error + "); the plain .acmi is kept").c_str());
            }
        } catch (...) {
            if (t.log) t.log("tacview: could not compress the recording (out of memory); the plain .acmi is kept");
        }
    }
}

}  // namespace

bool zip_queue_submit(const std::wstring& plain, ZipLogFn log, ZipDisplayFn display) {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_shut) return false;
    try {
        g_queue.push_back({plain, log, display});
        if (!g_running) {
            if (g_worker.joinable()) g_worker.join();  // it cleared g_running as its last act: returns at once
            g_running = true;
            g_worker = std::thread(worker_main);
        }
    } catch (...) {
        g_running = false;
        g_queue.clear();
        if (log) log("tacview: could not start the compressor thread; the plain .acmi is kept");
        return false;
    }
    return true;
}

void zip_queue_shutdown() {
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_shut = true;
    }
    g_cancel.store(true, std::memory_order_release);
    for (int i = 0; i < 100 && !zip_queue_idle(); ++i) Sleep(10);
    std::lock_guard<std::mutex> g(g_lock);
    if (g_worker.joinable()) g_worker.detach();
}

bool zip_queue_idle() {
    std::lock_guard<std::mutex> g(g_lock);
    return !g_running;
}

bool zip_queue_wait_idle(unsigned ms) {
    for (unsigned waited = 0; !zip_queue_idle(); waited += 5) {
        if (waited >= ms) return false;
        Sleep(5);
    }
    return true;
}

unsigned delete_stale_zip_temps(const std::wstring& dir) {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_running) return 0;
    unsigned n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.zip.acmi.tmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileW((dir + L"\\" + fd.cFileName).c_str())) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

void zip_test_fail_after(uint64_t bytes) { g_fail_after.store(bytes, std::memory_order_relaxed); }

}  // namespace lazysf2::acmi
