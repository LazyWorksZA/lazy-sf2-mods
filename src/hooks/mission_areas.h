// The target areas the running mission has populated (the game's own "near the mission" set), for the Tacview
// structures. Single missions only; fixed RVAs, July 2013 build only. See lazy-sf2-recon: recon-notes §20.6.
#pragma once

#include <cstddef>

namespace LazyWorks::SF2Patch::hooks {

enum class MissionAreas {
    Read,            // out holds the positions of the populated areas
    NotSingle,       // the running mission is not a single mission (campaign, instant action): no set
    Unavailable,     // unexpected build or memory: no set (logged once)
};

/// <summary>
/// Game thread, at a recording start. Copies the world x, y of each populated target area of the running single
/// mission into out (at most cap pairs; *n is the count). No allocation; every game access is guarded.
/// </summary>
MissionAreas mission_active_areas(double (*out)[2], size_t cap, size_t* n);

}  // namespace LazyWorks::SF2Patch::hooks
