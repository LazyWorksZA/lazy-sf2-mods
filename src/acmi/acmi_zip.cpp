#include "acmi/acmi_zip.h"

#include <windows.h>

#include <miniz.h>

#include <cstdio>
#include <cwchar>
#include <memory>
#include <vector>

namespace lazysf2::acmi {

namespace {

constexpr size_t kChunk = 256 * 1024;

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
    if (mz_zip_reader_get_num_files(&zip) != 1 || !mz_zip_reader_file_stat(&zip, 0, &st)) {
        error = "the zip does not hold exactly one entry";
    } else if (entry != st.m_filename || st.m_uncomp_size != plain_size || st.m_crc32 != plain_crc) {
        error = "the zip entry does not match the recording";
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

}  // namespace lazysf2::acmi
