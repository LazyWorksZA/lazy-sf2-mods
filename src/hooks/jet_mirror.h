#pragma once
// The players' jet controls (Phase 5): read from the owner's jet, written onto its avatars. The throttle and the gun
// triggers go into the avatar's input array, which nothing else writes on a REMOTEOBJECT aircraft, so its own engines
// and guns then drive the afterburner, nozzles, exhaust and bursts (visual-only: the rounds inherit REMOTEOBJECT).
// Game thread only. See lazy-sf2-recon: network-recon §11.2 and §11.12.
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks {

struct JetControls {
    float throttle = 0;        // input channel 3, 0..1
    uint8_t engines = 0;       // bit i: engine i runs (the first 8)
    uint8_t engine_count = 0;  // engines the bits describe (at most 8)
    uint8_t guns = 0;          // bit 0: input 12 (primary gun) above 0, bit 1: input 13 (secondary)
};

/// <summary>What a write changed, for log lines.</summary>
struct JetWriteResult {
    bool ok = false;              // the input array was written
    uint8_t engines_set = 0;      // engines whose running state was switched
    uint8_t engines_refused = 0;  // engines whose class or slot 22 is not the one we read
};

/// <summary>The owner's controls. False if AircraftObject is not the expected build or the object is not readable.</summary>
bool jet_read_controls(const void* obj, JetControls* out);
/// <summary>
/// Writes the throttle into the avatar's input channels 3..7 (the common and the per-engine throttles) and the gun
/// triggers into 12 and 13 (1 or 0), and switches engines 0..engine_count-1 whose running state differs through their
/// own slot 22. Engines past the owner's count are left as they are, and an engine on fire or failed (+0x40) is not
/// restarted.
/// </summary>
JetWriteResult jet_write_controls(void* obj, const JetControls& c);
/// <summary>A newly taken avatar: zeroes the gun and fire triggers the AI left in its input array, and returns the
/// engines' running state as taken (for jet_restore_engines). False if not readable.</summary>
bool jet_takeover(void* obj, JetControls* taken);
/// <summary>An avatar given back to the AI: its gun and fire triggers off, so it does not keep firing.</summary>
void jet_clear_triggers(void* obj);
/// <summary>Gives the engines back as they were taken, so the AI does not glide on. The throttle is left.</summary>
JetWriteResult jet_restore_engines(void* obj, uint8_t engines, uint8_t count);

/// <summary>One system of an aircraft, with its vtable as an AircraftObject RVA (0 if outside the image).</summary>
struct SysRef {
    unsigned char* sys = nullptr;
    uint32_t vt = 0;
    int comp = 0, index = 0;  // its component's index, and its index in that component's system list
};
/// <summary>The aircraft's systems in component order (at most max); SEH-guarded, stale pointers are skipped.</summary>
int jet_list_systems(void* obj, uintptr_t ao, SysRef* out, int max);

// The same with AircraftObject's base given (jet_mirror_core.cpp, no logging), so the tests can use a fake aircraft.
bool jet_read_controls_at(const void* obj, uintptr_t ao, JetControls* out);
JetWriteResult jet_write_controls_at(void* obj, uintptr_t ao, const JetControls& c, bool inputs);
bool jet_takeover_at(void* obj, uintptr_t ao, JetControls* taken);

}  // namespace LazyWorks::SF2Patch::hooks
