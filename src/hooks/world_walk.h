#pragma once
// The world walker: reads ground objects, vehicles, ships, flares and chaff (and tracer rounds) from the object
// manager's group table, for the Tacview recorder. Game thread only. Fixed RVAs, July 2013 build only.
// See lazy-sf2-recon: recon-notes §20.
#include <cstddef>
#include <cstdint>

#include "acmi/tacview_core.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The most records one world_walk_run() can produce.</summary>
constexpr size_t kWorldWalkMaxRecords = 4096 + 256 + 256 + 128;

/// <summary>
/// A recording starts: checks the build stamps of ObjectManager and the class DLLs and forgets everything from the
/// last flight (object pointers are per flight). Allocates the per-slot state. False if the walker stays off.
/// </summary>
bool world_walk_start();
/// <summary>
/// Reads the classes that are due at game time t into out (at most cap records, see kWorldWalkMaxRecords). Returns the
/// record count; *series_mask names the series the records list completely (0: nothing was walked, write no frame).
/// Does no allocation, I/O or logging in the walk loops.
/// </summary>
size_t world_walk_run(double t, acmi::TacviewRec* out, size_t cap, uint8_t* series_mask);
/// <summary>
/// The frame built from these records could not be written (ring full): keeps its Destroyed markers for the next
/// frame that lists the ground series. Beyond a small fixed number they are dropped and counted.
/// </summary>
void world_walk_frame_dropped(const acmi::TacviewRec* recs, size_t n);
/// <summary>The recording ended: logs the summary once.</summary>
void world_walk_stop();

}  // namespace LazyWorks::SF2Patch::hooks
