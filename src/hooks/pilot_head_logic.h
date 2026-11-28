#pragma once
// Pure mapping for the pilot head sync (pilot_head.cpp, world_sync.cpp): a received look direction to the angles
// written into a remote pilot's head target. No game memory. Unit tested in tests/net_tests.cpp.
// See lazy-sf2-recon: recon-notes §29 (pilot head).
#include <cstdint>

#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks::pilot_head {

// Sign of the game's pilot head angles against the view's (view yaw is positive to the left). Not confirmed in the
// running game: flip here if the heads turn the wrong way.
inline constexpr float kYawSign = 1.0f;
inline constexpr float kPitchSign = 1.0f;
// The game clamps a pilot's head to its own limits; this only keeps a bad value out of the pitch path.
inline constexpr float kMaxPitch = 1.0471976f;  // 60 degrees
inline constexpr float kMaxYaw = 3.0543262f;    // 175 degrees

/// <summary>The head target (radians) for a JetAnim's look bytes; false if the message carries no look.</summary>
inline bool target_from_anim(const net::JetAnimMsg& m, float* yaw, float* pitch) {
    if (!(m.flags & net::kAnimLook)) return false;
    float y = kYawSign * net::look_from_wire(m.look_yaw, net::kLookYawLimit);
    float p = kPitchSign * net::look_from_wire(m.look_pitch, net::kLookPitchLimit);
    if (y > kMaxYaw) y = kMaxYaw;
    if (y < -kMaxYaw) y = -kMaxYaw;
    if (p > kMaxPitch) p = kMaxPitch;
    if (p < -kMaxPitch) p = -kMaxPitch;
    *yaw = y;
    *pitch = p;
    return true;
}

/// <summary>True if a look received at received_ms is still usable at now_ms (older than fresh_ms is not).</summary>
inline bool look_fresh(uint64_t now_ms, uint64_t received_ms, uint64_t fresh_ms) {
    return now_ms >= received_ms && now_ms - received_ms <= fresh_ms;
}

}  // namespace LazyWorks::SF2Patch::hooks::pilot_head
