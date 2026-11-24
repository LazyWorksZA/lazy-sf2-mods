#include "acmi/tacview_writer.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace lazysf2::acmi {

namespace {
constexpr size_t kFlushBytes = 32 * 1024;
constexpr DWORD kFlushIdleMs = 200;
constexpr DWORD kPollMs = 20;

std::string narrow(const std::wstring& w) {
    std::string r;
    for (wchar_t c : w) r += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    return r;
}
}  // namespace

TacviewWriter::TacviewWriter(uint32_t ring_slots) {
    if (ring_slots < 16 || (ring_slots & (ring_slots - 1)) != 0) return;  // not a power of two: stays not ready
    ring_.reset(new (std::nothrow) TacviewRec[ring_slots]);
    if (ring_) cap_ = ring_slots;
}

void TacviewWriter::join_previous() {
    if (thread_.joinable()) thread_.join();  // it was told to stop by close(); a drain and a flush at most
}

std::wstring TacviewWriter::path() const {
    AcquireSRWLockShared(&path_lock_);
    std::wstring p = path_;
    ReleaseSRWLockShared(&path_lock_);
    return p;
}

bool TacviewWriter::open(const std::wstring& dir, const SYSTEMTIME& t, const char* utc_iso,
                         const TacviewCoreConfig& cfg, LogFn log, DisplayFn display, uint64_t max_bytes) {
    if (!ready() || !log || !utc_iso) return false;
    join_previous();
    if (open_.load()) return false;  // already open
    log_ = log;
    display_ = display;
    max_bytes_ = max_bytes;
    cfg_ = cfg;
    dir_ = dir;
    wchar_t nm[48];
    swprintf_s(nm, L"SF2 %04u-%02u-%02u %02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    base_name_ = nm;
    header_.clear();
    TacviewCore(cfg_).header(header_, utc_iso);

    file_ = nullptr;
    AcquireSRWLockExclusive(&path_lock_);
    path_.clear();
    ReleaseSRWLockExclusive(&path_lock_);
    head_.store(0);
    tail_.store(0);
    dropped_.store(0);
    written_.store(0);
    transforms_.store(0);
    t0_set_.store(false);
    last_ms_.store(-1);
    stop_.store(false);
    finished_.store(false);
    failed_.store(false);
    summary_logged_.store(false);
    created_.store(false);
    why_.store("closed");
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        log_("tacview: could not start the writer thread");
        return false;
    }
    open_.store(true, std::memory_order_release);
    return true;
}

bool TacviewWriter::begin_frame(size_t n) {
    if (!open_.load(std::memory_order_acquire) || failed_.load(std::memory_order_relaxed)) return false;
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    if (n + 1 > cap_ || h - t > cap_ - (n + 1)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TacviewWriter::commit_frame(size_t used, double t) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    ring_[(h + static_cast<uint32_t>(used)) & (cap_ - 1)].kind = TacviewRec::kEndFrame;
    ring_[(h + static_cast<uint32_t>(used)) & (cap_ - 1)].t = t;
    for (size_t i = 0; i < used; ++i) ring_[(h + static_cast<uint32_t>(i)) & (cap_ - 1)].kind = TacviewRec::kObject;
    head_.store(h + static_cast<uint32_t>(used) + 1, std::memory_order_release);
}

void TacviewWriter::close(bool wait_and_detach, const char* why) {
    if (!thread_.joinable()) return;
    if (why) why_.store(why, std::memory_order_relaxed);
    open_.store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    if (!wait_and_detach) return;
    // On process exit ExitProcess has already killed the writer thread: it will never set finished_. Do not wait
    // for it; write what is still in the ring from this thread, which is safe because the writer is gone.
    HANDLE th = static_cast<HANDLE>(thread_.native_handle());
    if (WaitForSingleObject(th, 0) == WAIT_OBJECT_0) {
        if (!finished_.load(std::memory_order_acquire)) flush_orphaned(why);
        thread_.join();  // already ended: returns at once
        return;
    }
    for (int i = 0; i < 100 && !finished_.load(std::memory_order_acquire); ++i) Sleep(10);
    thread_.detach();
}

void TacviewWriter::log_summary(uint64_t transforms, uint64_t bytes, const char* why) {
    if (summary_logged_.exchange(true)) return;
    char line[512];
    const std::string shown = narrow(display_ ? display_(path()) : path());
    std::snprintf(line, sizeof(line), "tacview: stopped (%s): %s, %llu transforms, %llu bytes, %u frame(s) dropped",
                  why ? why : "closed", shown.c_str(), static_cast<unsigned long long>(transforms),
                  static_cast<unsigned long long>(bytes), dropped_.load(std::memory_order_relaxed));
    log_(line);
}

// The writer thread was killed (process exit). Appends the ring's frames through a fresh core, up to the cap, and
// closes the file. Bounded: the ring holds at most cap_ records.
void TacviewWriter::flush_orphaned(const char* why) {
    HANDLE f = static_cast<HANDLE>(file_);
    if (!f) return;  // the thread died before it created the file: nothing was written
    try {
        TacviewCore core(cfg_);
        if (t0_set_.load(std::memory_order_acquire)) core.seed_time(t0_.load(), last_ms_.load());
        std::string buf;
        const uint64_t total = written_.load(std::memory_order_relaxed);
        if (total == 0) buf = header_;  // nothing was ever written
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        for (; tail != head; ++tail) {
            const TacviewRec& r = ring_[tail & (cap_ - 1)];
            if (r.kind == TacviewRec::kEndFrame) {
                const size_t before = buf.size();
                core.end_frame(r.t, buf);
                if (total + buf.size() > max_bytes_) {
                    buf.resize(before);
                    break;
                }
            } else {
                core.add(r);
            }
        }
        DWORD w = 0;
        if (!buf.empty()) WriteFile(f, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr);
        written_.store(total + w, std::memory_order_relaxed);
        transforms_.fetch_add(core.stats().transforms, std::memory_order_relaxed);
    } catch (...) {
    }
    CloseHandle(f);
    file_ = nullptr;
    log_summary(transforms_.load(), written_.load(), why);
}

void TacviewWriter::run() {
    char line[512];
    // 1. The folder and the file.
    if (!CreateDirectoryW(dir_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::snprintf(line, sizeof(line), "tacview: could not create the folder %s (error %lu); not recording",
                      narrow(display_ ? display_(dir_) : dir_).c_str(), GetLastError());
        log_(line);
        failed_.store(true, std::memory_order_release);
        finished_.store(true, std::memory_order_release);
        return;
    }
    HANDLE f = INVALID_HANDLE_VALUE;
    std::wstring path;
    for (unsigned n = 1; n < 100; ++n) {
        path = dir_ + L"\\" + base_name_;
        if (n > 1) path += L" (" + std::to_wstring(n) + L")";
        path += L".acmi";
        if (path.size() >= MAX_PATH) {  // CreateFileW without the \\?\ prefix is limited to MAX_PATH
            std::snprintf(line, sizeof(line), "tacview: output path too long (%zu characters, limit %d); not recording",
                          path.size(), static_cast<int>(MAX_PATH) - 1);
            log_(line);
            failed_.store(true, std::memory_order_release);
            finished_.store(true, std::memory_order_release);
            return;
        }
        f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
        if (f != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_FILE_EXISTS) continue;
        std::snprintf(line, sizeof(line), "tacview: could not create %s (error %lu); not recording",
                      narrow(display_ ? display_(path) : path).c_str(), GetLastError());
        log_(line);
        failed_.store(true, std::memory_order_release);
        finished_.store(true, std::memory_order_release);
        return;
    }
    if (f == INVALID_HANDLE_VALUE) {
        log_("tacview: no free file name in the Tacview folder; not recording");
        failed_.store(true, std::memory_order_release);
        finished_.store(true, std::memory_order_release);
        return;
    }
    AcquireSRWLockExclusive(&path_lock_);
    path_ = path;
    ReleaseSRWLockExclusive(&path_lock_);
    file_ = f;
    created_.store(true);
    std::snprintf(line, sizeof(line), "tacview: recording to %s (cap %llu bytes)",
                  narrow(display_ ? display_(path) : path).c_str(), static_cast<unsigned long long>(max_bytes_));
    log_(line);

    // 2. The loop.
    TacviewCore core(cfg_);
    std::string buf = header_;
    buf.reserve(kFlushBytes + 64 * 1024);
    uint64_t on_disk = 0;
    bool failed = false;  // a write failed: nothing more is written
    bool capped = false;  // the size cap was reached: what is buffered is still written
    DWORD last_flush = GetTickCount();

    auto flush = [&] {
        if (buf.empty()) return;
        if (!failed) {
            DWORD w = 0;
            if (!WriteFile(f, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr) || w != buf.size()) {
                failed = true;
                failed_.store(true, std::memory_order_release);
                std::snprintf(line, sizeof(line),
                              "tacview: write failed (error %lu); recording stops, the partial file is kept",
                              GetLastError());
                log_(line);
            } else {
                on_disk += w;
                written_.store(on_disk, std::memory_order_relaxed);
            }
        }
        buf.clear();
        last_flush = GetTickCount();
    };

    for (;;) {
        const bool stopping = stop_.load(std::memory_order_acquire);  // before the drain: covers all pushed so far
        const uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        while (tail != head) {
            const TacviewRec& r = ring_[tail & (cap_ - 1)];
            if (!failed && !capped) {
                if (r.kind == TacviewRec::kEndFrame) {
                    const size_t before = buf.size();
                    core.end_frame(r.t, buf);
                    if (on_disk + buf.size() > max_bytes_) {
                        buf.resize(before);
                        capped = true;
                        failed_.store(true, std::memory_order_release);
                        std::snprintf(line, sizeof(line), "tacview: reached the %llu byte cap; recording stops",
                                      static_cast<unsigned long long>(max_bytes_));
                        log_(line);
                    } else {
                        if (core.has_origin() && !t0_set_.load(std::memory_order_relaxed)) {
                            t0_.store(core.origin(), std::memory_order_relaxed);
                            t0_set_.store(true, std::memory_order_release);
                        }
                        last_ms_.store(core.last_ms(), std::memory_order_relaxed);
                        transforms_.store(core.stats().transforms, std::memory_order_relaxed);
                        if (buf.size() >= kFlushBytes) flush();
                    }
                } else {
                    core.add(r);
                }
            }
            ++tail;
            tail_.store(tail, std::memory_order_release);
        }
        if (stopping) break;
        if (!buf.empty() && GetTickCount() - last_flush >= kFlushIdleMs) flush();
        Sleep(kPollMs);
    }
    flush();
    CloseHandle(f);
    file_ = nullptr;
    log_summary(core.stats().transforms, on_disk, why_.load(std::memory_order_relaxed));
    finished_.store(true, std::memory_order_release);
}

}  // namespace lazysf2::acmi
