#pragma once
// Pure logic for the aircraft tracker (aircraft_track.cpp), kept free of Windows headers so the tests can use it.
#include <cstdint>

namespace lazysf2::hooks {

/// <summary>
/// An aircraft's side, from the object's side flag bits (FRIENDLY, ENEMY, NEUTRAL): the object manager hands out
/// one flag bit for each, and they live in the object's flag word.
/// </summary>
enum class AircraftSide : uint8_t {
    Unknown = 0,  // none of the three bits is set, or the masks are not known yet
    Friendly,
    Enemy,
    Neutral,
};

/// <summary>Classifies a flag word against the three masks. A mask of 0 (not filled in yet) never matches.</summary>
inline AircraftSide classify_side(uint32_t flags, uint32_t friendly, uint32_t enemy, uint32_t neutral) {
    if (friendly && (flags & friendly)) return AircraftSide::Friendly;
    if (enemy && (flags & enemy)) return AircraftSide::Enemy;
    if (neutral && (flags & neutral)) return AircraftSide::Neutral;
    return AircraftSide::Unknown;
}

/// <summary>The enable rule: the tracking hooks are installed only if a client needs them.</summary>
inline bool tracking_wanted(bool probe_on, bool tacview_recording_on) { return probe_on || tacview_recording_on; }

/// <summary>What the tracker does when a (new) AircraftObject.dll image appears. No aircraft can exist yet then.</summary>
struct LoadInput {
    bool hooked_patched;  // our hooks are on this very image and it still carries the patch
    bool build_ok;        // the image is the expected build
    bool wanted;          // tracking_wanted(...)
    bool had_old;         // hooks from an earlier image are registered
};
enum class LoadKind : uint8_t {
    Keep,      // already hooked here: nothing to do
    BadBuild,  // unexpected build: remember it, touch nothing
    Skip,      // nothing needs tracking: leave the image unhooked
    Install,   // hook the image
};
struct LoadDecision {
    LoadKind kind;
    bool drop_old;  // remove the earlier image's hooks first (only if that image is still the verified DLL)
};
inline LoadDecision decide_on_image(const LoadInput& in) {
    if (in.hooked_patched) return {LoadKind::Keep, false};
    if (!in.build_ok) return {LoadKind::BadBuild, false};
    return {in.wanted ? LoadKind::Install : LoadKind::Skip, in.had_old};
}

/// <summary>What the tracker does when the Options screen is left (menus: no aircraft exist, so changing the hooks is safe).</summary>
enum class OptionsExitAction : uint8_t { Nothing, Install, Remove };
inline OptionsExitAction decide_on_options_exit(bool dll_loaded, bool hooked, bool wanted) {
    if (wanted && !hooked && dll_loaded) return OptionsExitAction::Install;
    if (!wanted && hooked) return OptionsExitAction::Remove;
    return OptionsExitAction::Nothing;
}

}  // namespace lazysf2::hooks
