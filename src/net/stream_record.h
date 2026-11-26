// Writes the host's network stream to a file (format: stream_format.h, docs/stream-format.md).
//
// Threads: record() is called by ONE producer thread (the game thread) and only copies a few dozen bytes into a
// fixed ring: no lock, no allocation, no file I/O. A writer thread owned by the object drains the ring into a
// buffer and writes it to the file. If the ring is full the record is dropped and counted, never waited for.
// open() creates the file (one CreateFileW) and starts the thread; close() only signals it, so the game thread
// never waits for the disk. The thread flushes, closes the file and logs a summary. A later open() waits for the
// previous thread. Needs the Windows file API, but nothing of the game.
//
// Shutdown: close(true) waits at most 1 s for the writer and then detaches it. On a real FreeLibrary with a disk
// that stalls for longer than that, the detached thread could still run after the DLL is unloaded (a crash). That
// risk is accepted: it is a developer tool, the wait is bounded, and a join under the loader lock could deadlock.
// On process exit the thread has already been killed by ExitProcess: close(true) sees that, does not wait, and
// writes the ring's records itself. Only what the dead thread held in its own buffer (up to 32 KB, or 200 ms) is lost.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "net/stream_format.h"

namespace LazyWorks::SF2Patch::net {

class StreamWriter {
public:
    using LogFn = void (*)(const char* line);  // called from the writer thread, and from open()'s caller thread

    StreamWriter() = default;
    ~StreamWriter() { close(true); }
    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    /// <summary>
    /// Creates dir\PREFIX_NNN.lzst (CREATE_NEW, the first free NNN from 001; prefix "stream" on the host, "botserve"
    /// for the bot's --serve), writes the header and starts the writer thread. theatre may be null. False (and one log
    /// line) if no file could be created. max_bytes is the size cap for the whole file (kStreamMaxFileBytes in the
    /// game; the tests use a small one).
    /// </summary>
    bool open(const wchar_t* dir, double start_game_time, const char* theatre, LogFn log,
              uint64_t max_bytes = kStreamMaxFileBytes, const wchar_t* prefix = L"stream");
    static constexpr size_t kMaxPrefix = 16;  // characters
    bool is_open() const { return open_.load(std::memory_order_acquire); }

    /// <summary>Producer thread only. A no-op unless open. Payload is at most kStreamMaxPayload bytes.</summary>
    void record(StreamKind kind, double time, const void* payload, size_t len);

    /// <summary>
    /// Asks the thread to finish: it drains the ring, flushes and closes the file. Returns at once. With
    /// wait_and_detach (shutdown, possibly under the loader lock) it also waits at most 1 s for the thread to be
    /// done and then detaches it, because a join there can deadlock.
    /// </summary>
    void close(bool wait_and_detach);

    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t bytes_written() const { return written_.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kRingCap = 8192;  // records; a power of two
    struct Slot {
        uint8_t n;  // bytes used
        uint8_t data[kStreamRecordTotalMax];
    };
    static_assert((kRingCap & (kRingCap - 1)) == 0);
    static_assert(kStreamRecordTotalMax <= 255);

    void run();
    void join_previous();
    void flush_orphaned();

    std::thread thread_;
    void* file_ = nullptr;  // HANDLE, owned by the thread once started
    LogFn log_ = nullptr;
    uint64_t max_bytes_ = kStreamMaxFileBytes;
    uint8_t header_[sizeof(StreamHeader)] = {};
    wchar_t name_[kMaxPrefix + 16] = {};  // the file's name, for the log (ASCII)

    std::atomic<bool> open_{false};      // accepting records
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};  // the thread has closed the file and is about to return
    std::atomic<uint32_t> head_{0}, tail_{0};  // producer writes head_, consumer tail_ (free-running)
    std::atomic<uint32_t> dropped_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<bool> capped_{false};
    Slot ring_[kRingCap];
};

}  // namespace LazyWorks::SF2Patch::net
