#pragma once
// The players' jet controls (Phase 5): read from the owner's jet, written onto its avatars. The throttle goes into the
// avatar's input array, which nothing else writes on a REMOTEOBJECT aircraft, so its own engines then drive the
// afterburner, nozzles and exhaust natively. Game thread only. See lazy-sf2-recon: network-recon §11.2 and §11.12.
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks {

struct JetControls {
    float throttle = 0;        // input channel 3, 0..1
    uint8_t engines = 0;       // bit i: engine i runs (the first 8)
    uint8_t engine_count = 0;  // engines the bits describe (at most 8)
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
/// Writes the throttle into the avatar's input channels 3..7 (the common and the per-engine throttles), and switches
/// engines 0..engine_count-1 whose running state differs through their own slot 22. Engines past the owner's count
/// are left as they are, and an engine on fire or failed (+0x40) is not restarted.
/// </summary>
JetWriteResult jet_write_controls(void* obj, const JetControls& c);
/// <summary>A newly taken avatar: zeroes the gun and fire triggers the AI left in its input array, and returns the
/// engines' running state as taken (for jet_restore_engines). False if not readable.</summary>
bool jet_takeover(void* obj, JetControls* taken);
/// <summary>Gives the engines back as they were taken, so the AI does not glide on. The throttle is left.</summary>
JetWriteResult jet_restore_engines(void* obj, uint8_t engines, uint8_t count);

// The same with AircraftObject's base given (jet_mirror_core.cpp, no logging), so the tests can use a fake aircraft.
bool jet_read_controls_at(const void* obj, uintptr_t ao, JetControls* out);
JetWriteResult jet_write_controls_at(void* obj, uintptr_t ao, const JetControls& c, bool throttle);
bool jet_takeover_at(void* obj, uintptr_t ao, JetControls* taken);

}  // namespace LazyWorks::SF2Patch::hooks
