#pragma once
// Remote avatars (developer tool, part of the probe): an AI aircraft taken over to show a remote player's jet.
// REMOTEOBJECT switches off its AI and forces; we are then its only writer. Game thread only, and the writes only
// from that aircraft's own step callback. Used by the host (src/net/host.cpp) and, later, the mod client.
// See lazy-sf2-recon: network-recon §4.2 and §8.2, and the Phase 4 plan, section 2.6.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hooks/remote_avatar_logic.h"
#include "hooks/replay_math.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>Copies n values of T from obj + off (an aircraft field). Game thread, on a live aircraft only.</summary>
template <typename T>
void obj_read(const void* obj, size_t off, T* out, size_t n) {
    std::memcpy(out, static_cast<const char*>(obj) + off, n * sizeof(T));
}
template <typename T>
void obj_write(void* obj, size_t off, const T* in, size_t n) {
    std::memcpy(static_cast<char*>(obj) + off, in, n * sizeof(T));
}

/// <summary>What to take: the remote player's aircraft type, and where to put the avatar (world metres, radians, m/s).</summary>
struct AvatarPick {
    const char* want_type;
    const double place[3];
    float heading;
    float vel[3];
};

/// <summary>What avatar_take chose, for the caller's log line.</summary>
struct AvatarInfo {
    char name[64] = {};  // printable ASCII
    char type[32] = {};  // empty when unknown
    AvatarTier tier = AvatarTier::None;
    int flight_no = -1;
    double dist = 0;     // from the player, before the move
    double from[3] = {};  // where it was
};

/// <summary>
/// Game thread. Takes a free AI aircraft (plan 2.6 preference order; never the player, never one in taken), sets
/// REMOTEOBJECT, zeroes its rates and places it at pick.place, level on pick.heading, with pick.vel. False if none
/// (or the REMOTEOBJECT mask can't be read); why says what was chosen and why, or why nothing was.
/// </summary>
bool avatar_take(const AvatarPick& pick, const AvatarHandle* taken, int n_taken, AvatarHandle* out, char* why,
                 size_t why_cap, AvatarInfo* info = nullptr);

/// <summary>Writes position, velocity, rates, matrix and angles. From that aircraft's own step callback only.</summary>
void avatar_write(const AvatarHandle& h, const replay_math::Kin& k);

/// <summary>
/// Zeroes the angular rates, so that REMOTEOBJECT's native coasting goes straight on with the attitude held. Same
/// thread rule as avatar_write: call it once when the writes stop (Coast).
/// </summary>
void avatar_zero_rates(const AvatarHandle& h);

/// <summary>Sets our REMOTEOBJECT bits again if the game cleared them. True if it had to. Same thread rule as avatar_write.</summary>
bool avatar_keep_remote(const AvatarHandle& h);

/// <summary>Gives the aircraft back: if it is live, clears only set_bits (the AI takes over). Then empties h.</summary>
void avatar_release(AvatarHandle& h, bool live);

/// <summary>live[i]: h[i] (pointer and seq) is still in the probe's aircraft table. An empty handle is not live.</summary>
void avatar_check_live(const AvatarHandle* h, int n, bool* live);

/// <summary>True if h holds this aircraft (a step callback's pointer).</summary>
inline bool avatar_is(const AvatarHandle& h, const void* aircraft) { return h.obj && h.obj == aircraft; }

}  // namespace LazyWorks::SF2Patch::hooks
