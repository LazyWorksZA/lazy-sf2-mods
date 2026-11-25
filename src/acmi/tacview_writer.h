// Writes a Tacview recording (ACMI 2.2 text) from frames of aircraft states. Follows the pattern of
// net/stream_record.h: one producer thread (the game thread) copies frames into a fixed ring (no lock, no
// allocation, no file I/O), and a writer thread owned by the object formats them (tacview_core.h) and writes the file.
//
// open() allocates nothing on the game thread beyond the header text and starts the thread. The thread creates the
// folder and the file (CREATE_NEW; on a name clash it appends " (2)" and so on), so a slow or failing disk never
// stalls the game. If the ring is full a whole frame is dropped and counted, never waited for.
//
// Failure: any failure (folder, file, write) is logged once by the thread and sets failed(); accepting() turns
// false, the producer should stop and call close(). The size cap does the same. The partial file is kept.
//
// Zip: when a recording ends through close(false) (a normal stop or the cap, not a failed write), the thread closes the
// plain file, logs the summary and queues the path for the background compressor (acmi_zip.h), then finishes: idle()
// does not wait for the zip. The compressor packs it into "<name>.zip.acmi" and deletes the plain file; any failure
// keeps the plain file and is logged once. close(true) (shutdown) and the orphan path never queue a zip. A recording
// start deletes stale "*.zip.acmi.tmp" files of an earlier session.
//
// Exit: close(true) is for the DLL's shutdown. On process exit ExitProcess has killed the writer thread already:
// close(true) sees that, does not wait, and appends the ring's frames itself (with a fresh core seeded with the
// time origin, so the file stays valid). Only what the dead thread held in its own buffer (up to 32 KB or 200 ms) is
// lost. On FreeLibrary it waits at most 1 s for the thread and then detaches it (see net/stream_record.h).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "acmi/tacview_core.h"

namespace LazyWorks::SF2Patch::acmi {

constexpr uint64_t kTacviewMaxFileBytes = 512ull * 1024 * 1024;

class TacviewWriter {
public:
    using LogFn = void (*)(const char* line);               // called from the writer thread (and from close(true))
    using DisplayFn = std::wstring (*)(const std::wstring&);  // path to show in the log; may be null

    /// <summary>ring_slots must be a power of two. The ring is allocated here (the first recording), not at load.</summary>
    explicit TacviewWriter(uint32_t ring_slots = 8192);
    ~TacviewWriter() { close(true, "destroyed"); }
    TacviewWriter(const TacviewWriter&) = delete;
    TacviewWriter& operator=(const TacviewWriter&) = delete;

    /// <summary>
    /// True when no writer thread is running or it has finished (so open() will not wait). Game thread: check this
    /// before open(), and retry later when false.
    /// </summary>
    bool idle() const;
    /// <summary>For tests: asks the thread to finish and waits for it, however long it takes.</summary>
    void close_and_join(const char* why = "closed");

    bool ready() const { return ring_ != nullptr; }  // false if the ring could not be allocated

    /// <summary>
    /// Starts a recording in folder dir (the Tacview folder; created if missing). local_now names the file. The
    /// header's ReferenceTime is utc_iso. False if one is still open or the thread could not start.
    /// </summary>
    bool open(const std::wstring& dir, const SYSTEMTIME& local_now, const char* utc_iso, const TacviewCoreConfig& cfg,
              LogFn log, DisplayFn display, uint64_t max_bytes = kTacviewMaxFileBytes);
    /// <summary>Open and not failed or capped: the producer should keep going.</summary>
    bool accepting() const { return open_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire); }

    /// <summary>
    /// Producer thread only. Reserves n object slots plus the end marker for one frame; false if the ring cannot
    /// hold the frame (counted as dropped) or nothing is open. Fill slot(0..n-1), then commit_frame.
    /// </summary>
    bool begin_frame(size_t n);
    /// <summary>Producer thread only. True when begin_frame(n) would succeed now; nothing is counted.</summary>
    bool can_take(size_t n) const;
    /// <summary>The ring arithmetic of begin_frame and can_take: n records plus the end marker fit (free-running indexes).</summary>
    static bool ring_fits(uint32_t head, uint32_t tail, uint32_t cap, size_t n);
    TacviewRec& slot(size_t i) { return ring_[(head_.load(std::memory_order_relaxed) + static_cast<uint32_t>(i)) & (cap_ - 1)]; }
    /// <summary>
    /// Publishes the frame: the first `used` reserved slots (used &lt;= n) and an end marker at game time t. series_mask
    /// names the series the frame lists completely (see TacviewCore::end_frame).
    /// </summary>
    void commit_frame(size_t used, double t, uint8_t series_mask = TacviewRec::kSeriesAll);

    /// <summary>
    /// Asks the thread to finish: it drains the ring, flushes and closes the file, then logs the summary (with why, a
    /// string literal). Returns at once, except wait_and_detach (see above).
    /// </summary>
    void close(bool wait_and_detach, const char* why);

    uint32_t dropped_frames() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t bytes_written() const { return written_.load(std::memory_order_relaxed); }
    /// <summary>The plain .acmi path (after the thread has created it), empty before, empty before. Not for the game thread's hot path.</summary>
    std::wstring path() const;

private:
    void run();
    void join_previous();
    void flush_orphaned(const char* why);
    void log_summary(uint64_t transforms, uint64_t bytes, const char* why, uint64_t structures = 0,
                     uint64_t structures_skipped = 0);

    std::thread thread_;
    std::unique_ptr<TacviewRec[]> ring_;
    uint32_t cap_ = 0;
    LogFn log_ = nullptr;
    DisplayFn display_ = nullptr;
    uint64_t max_bytes_ = kTacviewMaxFileBytes;
    TacviewCoreConfig cfg_;
    std::wstring dir_;
    std::wstring base_name_;  // "SF2 yyyy-mm-dd HH-MM-SS"
    std::string header_;
    void* file_ = nullptr;  // HANDLE, set by the thread, used by the orphan path only after the thread is dead
    std::wstring path_;     // guarded by path_lock_
    mutable SRWLOCK path_lock_ = SRWLOCK_INIT;

    std::atomic<bool> open_{false};      // accepting frames
    std::atomic<bool> failed_{false};    // failure or cap: the producer should stop
    std::atomic<bool> stop_{false};
    std::atomic<bool> no_zip_{false};  // set by close(true)
    std::atomic<bool> finished_{false};  // the thread has closed the file and is about to return
    std::atomic<const char*> why_{"closed"};
    std::atomic<uint32_t> head_{0}, tail_{0};  // producer writes head_, consumer tail_ (free-running)
    std::atomic<uint32_t> dropped_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> transforms_{0};
    std::atomic<bool> t0_set_{false};
    std::atomic<double> t0_{0};
    std::atomic<long long> last_ms_{-1};
    std::atomic<uint32_t> next_explosion_{1};  // the writer core's next explosion number, for flush_orphaned
    std::atomic<bool> summary_logged_{false};
    std::atomic<bool> created_{false};  // the file exists
};

}  // namespace LazyWorks::SF2Patch::acmi
