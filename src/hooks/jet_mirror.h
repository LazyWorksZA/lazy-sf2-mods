#pragma once
// The players' jet controls (Phase 5): read from the owner's jet, written onto its avatars. The throttle goes into the
// avatar's input array, which nothing else writes on a REMOTEOBJECT aircraft, so its own engines then drive the
// afterburner, nozzles and exhaust natively. Game thread only. See lazy-sf2-recon:
// network-recon §11.2 and §11.4.
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks {

struct JetControls {
    float throttle = 0;   // input channel 3, 0..1
    uint8_t engines = 0;  // bit i: engine i runs (the first 8)
    uint8_t engine_count = 0;
};

/// <summary>The owner's controls. False if AircraftObject is not the expected build or the object is not readable.</summary>
bool jet_read_controls(const void* obj, JetControls* out);

/// <summary>What jet_write_controls changed, for log lines.</summary>
struct JetWriteResult {
    bool ok = false;           // the input array was written
    uint8_t engines_set = 0;   // engines whose running state was switched
    uint8_t engines_refused = 0;  // engines whose class or slot 22 is not the one we read
};
/// <summary>Writes the throttle into the avatar's input array, and switches engines whose running state differs
/// through their own slot 22. From the avatar's step (or a message handled on the game thread).</summary>
JetWriteResult jet_write_controls(void* obj, const JetControls& c);

/// <summary>A newly taken avatar: zero the gun and fire triggers the AI left in its input array.</summary>
void jet_takeover(void* obj);

}  // namespace LazyWorks::SF2Patch::hooks
