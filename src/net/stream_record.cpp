#include "net/stream_record.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace LazyWorks::SF2Patch::net {

namespace {
constexpr size_t kBufBytes = 64 * 1024;
constexpr size_t kFlushBytes = 32 * 1024;
constexpr DWORD kFlushIdleMs = 200;
constexpr DWORD kPollMs = 10;
}  // namespace

void StreamWriter::join_previous() {
    if (thread_.joinable()) thread_.join();  // it was told to stop by close(); a drain and a flush at most
}

bool StreamWriter::open(const wchar_t* dir, double start_game_time, const char* theatre, LogFn log,
                        uint64_t max_bytes, const wchar_t* prefix) {
    if (!dir || !log || !prefix || !*prefix || wcslen(prefix) > kMaxPrefix) return false;
    join_previous();
    if (open_.load()) return false;  // already open
    log_ = log;
    max_bytes_ = max_bytes;
    char line[160];

    if (!encode_stream_header(start_game_time, theatre, header_, sizeof(header_))) return false;
    HANDLE f = INVALID_HANDLE_VALUE;
    for (unsigned n = 1; n < 1000; ++n) {
        swprintf_s(name_, L"%ls_%03u.lzst", prefix, n);  // at most kMaxPrefix + 9 characters: cannot truncate
        const std::wstring path = std::wstring(dir) + L"\\" + name_;
        if (path.size() >= MAX_PATH) {  // CreateFileW without the \\?\ prefix is limited to MAX_PATH
            std::snprintf(line, sizeof(line), "stream: output path too long (%zu characters, limit %d)", path.size(),
                          static_cast<int>(MAX_PATH) - 1);
            log_(line);
            return false;
        }
        f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
        if (f != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_FILE_EXISTS) continue;
        std::snprintf(line, sizeof(line), "stream: could not create %ls (error %lu)", name_, GetLastError());
        log_(line);
        return false;
    }
    if (f == INVALID_HANDLE_VALUE) {
        std::snprintf(line, sizeof(line), "stream: no free %ls_NNN.lzst name in the folder", prefix);
        log_(line);
        return false;
    }
    file_ = f;
    head_.store(0);
    tail_.store(0);
    dropped_.store(0);
    written_.store(0);
    capped_.store(false);
    stop_.store(false);
    finished_.store(false);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        CloseHandle(f);
        file_ = nullptr;
        log_("stream: could not start the writer thread");
        return false;
    }
    open_.store(true, std::memory_order_release);
    std::snprintf(line, sizeof(line), "stream: recording to %ls (cap %llu bytes)", name_,
                  static_cast<unsigned long long>(max_bytes_));
    log_(line);
    return true;
}

void StreamWriter::record(StreamKind kind, double time, const void* payload, size_t len) {
    if (!open_.load(std::memory_order_acquire) || capped_.load(std::memory_order_relaxed)) return;
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= kRingCap) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Slot& s = ring_[h & (kRingCap - 1)];
    const size_t n = encode_stream_record(kind, time, static_cast<const uint8_t*>(payload), len, s.data, sizeof(s.data));
    if (!n) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.n = static_cast<uint8_t>(n);
    head_.store(h + 1, std::memory_order_release);
}

void StreamWriter::close(bool wait_and_detach) {
    if (!thread_.joinable()) return;
    open_.store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    if (!wait_and_detach) return;
    // On process exit ExitProcess has already killed the writer thread: it will never set finished_. Do not wait
    // for it; write what is still in the ring from this thread, which is safe because the writer is gone.
    HANDLE th = static_cast<HANDLE>(thread_.native_handle());
    if (WaitForSingleObject(th, 0) == WAIT_OBJECT_0) {
        if (!finished_.load(std::memory_order_acquire)) flush_orphaned();
        thread_.join();  // already ended: returns at once, takes no loader-lock-dependent wait
        return;
    }
    for (int i = 0; i < 100 && !finished_.load(std::memory_order_acquire); ++i) Sleep(10);
    thread_.detach();
}

// The writer thread was killed (process exit). Appends the ring's records, up to the cap, and closes the file.
// Bounded: the ring holds at most kRingCap records (about 1 MB). Whatever the dead thread had in its own buffer
// (at most 32 KB, or 200 ms of records) is lost.
void StreamWriter::flush_orphaned() {
    HANDLE f = static_cast<HANDLE>(file_);
    if (!f) return;
    std::vector<uint8_t> buf;
    uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    uint64_t total = written_.load(std::memory_order_relaxed);
    if (total == 0) buf.insert(buf.end(), header_, header_ + sizeof(header_));  // nothing was ever written
    for (; tail != head; ++tail) {
        const Slot& s = ring_[tail & (kRingCap - 1)];
        if (total + buf.size() + s.n > max_bytes_) break;
        buf.insert(buf.end(), s.data, s.data + s.n);
    }
    DWORD w = 0;
    if (!buf.empty()) WriteFile(f, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr);
    CloseHandle(f);
    file_ = nullptr;
}

void StreamWriter::run() {
    std::vector<uint8_t> buf;
    buf.reserve(kBufBytes + kStreamRecordTotalMax);
    buf.insert(buf.end(), header_, header_ + sizeof(header_));
    HANDLE f = static_cast<HANDLE>(file_);
    uint64_t on_disk = 0;
    uint64_t records = 0;
    bool failed = false;
    bool logged_cap = false;
    uint32_t dropped_logged = 0;
    DWORD last_flush = GetTickCount();
    DWORD last_drop_log = last_flush;
    char line[160];

    auto flush = [&] {
        if (buf.empty()) return;
        if (!failed) {
            DWORD w = 0;
            if (!WriteFile(f, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr) || w != buf.size()) {
                failed = true;
                std::snprintf(line, sizeof(line),
                              "stream: write to %ls failed (error %lu); recording stops, the partial file is kept",
                              name_, GetLastError());
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
            const Slot& s = ring_[tail & (kRingCap - 1)];
            if (!failed && !capped_.load(std::memory_order_relaxed)) {
                if (on_disk + buf.size() + s.n > max_bytes_) {
                    capped_.store(true, std::memory_order_relaxed);
                    if (!logged_cap) {
                        logged_cap = true;
                        std::snprintf(line, sizeof(line), "stream: %ls reached the %llu byte cap; recording stops", name_,
                                      static_cast<unsigned long long>(max_bytes_));
                        log_(line);
                    }
                } else {
                    buf.insert(buf.end(), s.data, s.data + s.n);
                    ++records;
                    if (buf.size() >= kBufBytes) flush();
                }
            }
            ++tail;
            tail_.store(tail, std::memory_order_release);
        }
        if (stopping) break;
        const DWORD now = GetTickCount();
        if (buf.size() >= kFlushBytes || (!buf.empty() && now - last_flush >= kFlushIdleMs)) flush();
        const uint32_t d = dropped_.load(std::memory_order_relaxed);
        if (d != dropped_logged && now - last_drop_log >= 1000) {
            std::snprintf(line, sizeof(line), "stream: the writer fell behind; %u record(s) dropped so far", d);
            log_(line);
            dropped_logged = d;
            last_drop_log = now;
        }
        Sleep(kPollMs);
    }
    flush();
    CloseHandle(f);
    file_ = nullptr;
    std::snprintf(line, sizeof(line), "stream: closed %ls: %llu records, %llu bytes, %u dropped", name_,
                  static_cast<unsigned long long>(records), static_cast<unsigned long long>(on_disk),
                  dropped_.load(std::memory_order_relaxed));
    log_(line);
    finished_.store(true, std::memory_order_release);
}

}  // namespace LazyWorks::SF2Patch::net
