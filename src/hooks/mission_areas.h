// The target areas and placed objects of the running single mission, for the Tacview structures: which areas the game
// populated, which placed objects it drew as plain models, and their damage. Fixed RVAs, July 2013 build only.
// See lazy-sf2-recon: recon-notes §20.6.
#pragma once

#include <cstddef>
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks {

enum class MissionAreas {
    Read,         // out holds the populated areas
    NotSingle,    // the running mission is not a single mission (campaign, instant action): no set
    Unavailable,  // unexpected build or memory: no set (logged once)
};

/// <summary>A populated target area of the running single mission.</summary>
struct GameArea {
    double x = 0, y = 0;
    const void* const* placed = nullptr;  // its placed-object records, in target order (game memory)
    int n_placed = 0;
};

/// <summary>
/// Game thread, at a recording start. Copies each populated target area of the running single mission into out (at
/// most cap; *n is the count). No allocation; every game access is guarded.
/// </summary>
MissionAreas mission_active_areas(GameArea* out, size_t cap, size_t* n);

/// <summary>One placed object as the mission holds it.</summary>
struct PlacedView {
    double x = 0, y = 0;  // world metres
    bool model = false;   // the mission drew it as a plain model this flight
    bool object = false;  // the mission made it a live object instead
    const void* record = nullptr;
};

/// <summary>
/// Game thread. True while the running mission is still the one the last mission_active_areas() call read as Read: the
/// registry's MSN is the same SingleMission object and its area array and count are unchanged. The records it gave
/// are only read while this holds.
/// </summary>
bool mission_unchanged();

/// <summary>Game thread. Reads placed object i of area a; false when it cannot be read.</summary>
bool mission_placed(const GameArea& a, int i, PlacedView* out);

/// <summary>
/// Game thread. For each record whose dead[i] is 0, sets it to 1 when its damage has reached destroyed, and lists i in
/// out (at most cap). An unreadable record counts as not destroyed. Returns the number listed.
/// </summary>
size_t mission_poll_destroyed(const void* const* records, size_t n, uint8_t* dead, uint32_t* out, size_t cap);

}  // namespace LazyWorks::SF2Patch::hooks
