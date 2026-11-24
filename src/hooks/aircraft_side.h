#pragma once
// Pure logic for the aircraft tracker (aircraft_track.cpp), kept free of Windows headers so the tests can use it.
#include <cstdint>

namespace lazysf2::hooks {

// An aircraft's side as the game shows it to the player. The object manager hands out one flag bit each for
// FRIENDLY, ENEMY and NEUTRAL; the game keeps them in the object's flag word and treats them as the local view
// (it keeps them when it loads a saved state, and leaves them out of the state it saves).
enum class AircraftSide : uint8_t {
    Unknown = 0,  // none of the three bits is set, or the masks are not known yet
    Friendly,
    Enemy,
    Neutral,
};

// Classifies a flag word against the three masks. A mask of 0 (not filled in yet) never matches.
inline AircraftSide classify_side(uint32_t flags, uint32_t friendly, uint32_t enemy, uint32_t neutral) {
    if (friendly && (flags & friendly)) return AircraftSide::Friendly;
    if (enemy && (flags & enemy)) return AircraftSide::Enemy;
    if (neutral && (flags & neutral)) return AircraftSide::Neutral;
    return AircraftSide::Unknown;
}

// The enable rule: the tracking hooks are installed only if a client needs them.
inline bool tracking_wanted(bool probe_on, bool tacview_recording_on) { return probe_on || tacview_recording_on; }

}  // namespace lazysf2::hooks
