// Packs a finished recording into a Tacview-readable "<name>.zip.acmi" (a standard zip with one deflated entry
// "<name>.acmi"). Uses miniz and streams from the file, so a 512 MB recording is never held in memory. Pure Win32 and
// miniz: no game dependencies.
#pragma once

#include <cstdint>
#include <string>

namespace lazysf2::acmi {

/// <summary>
/// Compresses plain (a path ending in ".acmi") to the same path with ".zip.acmi". It writes "<zip>.tmp", reopens it and
/// decompresses the entry (streamed) and checks its name, size and CRC-32 against the plain file, renames it into place (never over an existing file),
/// and only then deletes plain. On any failure the plain file is kept, the temp file is removed, and error says why.
/// </summary>
/// <remarks>Aborts (plain file kept) soon after zip_queue_shutdown().</remarks>
/// <param name="zip_path">The final path on success.</param>
/// <param name="zip_bytes">The zip's size on success.</param>
bool compress_acmi(const std::wstring& plain, std::wstring& zip_path, uint64_t& zip_bytes, std::string& error);

using ZipLogFn = void (*)(const char* line);
using ZipDisplayFn = std::wstring (*)(const std::wstring&);  // path to show in the log; may be null

/// <summary>
/// Hands a closed recording to the background compressor (one worker thread that runs while the queue is not empty).
/// It only queues: it never compresses at once and never waits for a zip. The worker logs one line per file: packed,
/// or failed with the plain file kept. False (nothing queued) after zip_queue_shutdown or when the worker cannot start.
/// </summary>
bool zip_queue_submit(const std::wstring& plain, ZipLogFn log, ZipDisplayFn display);
/// <summary>
/// Shutdown (DllMain): refuses new work, makes the running zip abort, waits at most 1 s, then detaches the worker. Never
/// joins (loader lock). At process exit the worker is already dead and nothing waits.
/// </summary>
void zip_queue_shutdown();
/// <summary>True when no zip is queued or running.</summary>
bool zip_queue_idle();
/// <summary>For tests: waits until the queue is idle, up to ms.</summary>
bool zip_queue_wait_idle(unsigned ms);
/// <summary>
/// Deletes "*.zip.acmi.tmp" in dir (leftovers of a zip cut short by a process exit). Does nothing while the compressor is
/// busy, because it may own one. Returns how many were deleted.
/// </summary>
unsigned delete_stale_zip_temps(const std::wstring& dir);
/// <summary>For tests: makes zips fail once they have read this many bytes (0 turns it off).</summary>
void zip_test_fail_after(uint64_t bytes);

}  // namespace lazysf2::acmi
