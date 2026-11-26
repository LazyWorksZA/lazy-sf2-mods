// Remote avatars: taking over an AI aircraft, driving it, and giving it back. See remote_avatar.h.
#include "hooks/remote_avatar.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/jet_mirror.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

using replay_math::build_matrix;

constexpr DWORD kAircraftTimestamp = 0x51d220bd, kAircraftImageSize = 0xc2000;
constexpr DWORD kRemoteMaskRva = 0xb7b28;  // AircraftObject.dll global: the REMOTEOBJECT flag mask
constexpr size_t kOffFlags = 0xc, kOffPos = 0x38, kOffVel = 0x50, kOffRates = 0xd0, kOffMatrix = 0xdc,
                 kOffAngles = 0x100;

// Game thread only. The mask is read once; a build mismatch is checked (and logged) once.
uint32_t g_mask = 0;
bool g_mask_tried = false;

uint32_t remote_mask() {
    if (g_mask || g_mask_tried) return g_mask;
    const HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (!m) return 0;  // not loaded yet: try again next time
    g_mask_tried = true;
    if (is_expected_build(m, kAircraftTimestamp, kAircraftImageSize, "remote avatar: AircraftObject.dll"))
        std::memcpy(&g_mask, at_rva(m, kRemoteMaskRva), sizeof(g_mask));
    return g_mask;
}

struct TakeCtx {
    // in
    const AvatarPick* pick;
    const AvatarHandle* taken;
    int n_taken;
    uint32_t mask;
    // out
    bool found = false;
    void* obj = nullptr;
    unsigned long long seq = 0;
    uint32_t orig_flags = 0;
    AvatarRank rank;
    double from[3] = {};
    wchar_t name[64] = {};
    char type[32] = {};
};

void take_cb(const LiveAircraft* list, size_t n, void* vctx) {  // under the shared lock: no logging, no allocation
    auto& c = *static_cast<TakeCtx*>(vctx);
    const LiveAircraft* player = nullptr;
    for (size_t i = 0; i < n; ++i)
        if (list[i].is_player) player = &list[i];
    // Distances from where the avatar goes (the join position on the host, the remote's State on the client); from the
    // player only if the placement is not a finite position.
    double ref[3];
    const double* pl = c.pick->place;
    if ((std::isfinite(pl[0]) && std::isfinite(pl[1]) && std::isfinite(pl[2])) || !player) {
        std::memcpy(ref, pl, sizeof(ref));
    } else {
        obj_read(player->obj, kOffPos, ref, 3);
    }
    size_t chosen = n;
    AvatarRank rank;
    if (c.pick->prefer) {
        for (size_t i = 0; i < n && chosen == n; ++i)
            if (list[i].obj == c.pick->prefer && !list[i].is_player &&
                !avatar_logic::is_taken(c.taken, c.n_taken, list[i].obj, list[i].seq))
                chosen = i;
        if (chosen < n) {
            rank.tier = AvatarTier::Wingman;
            rank.seq = list[chosen].seq;
            rank.nearby = true;
        }
    }
    if (chosen == n) {
        AvatarPicker picker(player ? player->name : nullptr, c.pick->want_type);
        for (size_t i = 0; i < n; ++i) {
            double p[3];
            obj_read(list[i].obj, kOffPos, p, 3);
            picker.offer(i, avatar_candidate(list[i], p, ref, c.taken, c.n_taken));
        }
        if (!picker.found()) return;
        chosen = picker.index();
        rank = picker.best();
    }
    const LiveAircraft& a = list[chosen];
    c.found = true;
    c.obj = a.obj;
    c.seq = a.seq;
    c.rank = rank;
    obj_read(c.obj, kOffPos, c.from, 3);
    if (a.name) wcsncpy_s(c.name, a.name, _TRUNCATE);
    if (a.type_name) strncpy_s(c.type, a.type_name, _TRUNCATE);
    // REMOTEOBJECT on, rates zero, at the place: the aircraft coasts and we are the only writer. Done under the lock,
    // so the aircraft cannot be destroyed between the choice and the writes.
    char* obj = static_cast<char*>(c.obj);
    obj_read(obj, kOffFlags, &c.orig_flags, 1);
    const float zero[3] = {};
    obj_write(obj, kOffRates, zero, 3);
    obj_write(obj, kOffVel, c.pick->vel, 3);
    const uint32_t flags = c.orig_flags | c.mask;
    obj_write(obj, kOffFlags, &flags, 1);
    obj_write(obj, kOffPos, c.pick->place, 3);
    const double ang[3] = {0.0, static_cast<double>(c.pick->heading), 0.0};
    const float angf[3] = {0.0f, c.pick->heading, 0.0f};
    float m[9];
    build_matrix(ang, m);
    obj_write(obj, kOffMatrix, m, 9);
    obj_write(obj, kOffAngles, angf, 3);
}

struct LiveCtx {
    const AvatarHandle* h;
    int n;
    bool* live;
};
void live_cb(const LiveAircraft* list, size_t n, void* vctx) {  // under the shared lock: no logging
    auto& c = *static_cast<LiveCtx*>(vctx);
    for (size_t i = 0; i < n; ++i)
        for (int s = 0; s < c.n; ++s)
            if (avatar_logic::is_taken(&c.h[s], 1, list[i].obj, list[i].seq)) c.live[s] = true;
}

void put_why(char* why, size_t cap, const char* text) {
    if (why && cap) strncpy_s(why, cap, text, _TRUNCATE);
}

}  // namespace

bool avatar_take(const AvatarPick& pick, const AvatarHandle* taken, int n_taken, AvatarHandle* out, char* why,
                 size_t why_cap, AvatarInfo* info) {
    *out = AvatarHandle{};
    const uint32_t mask = remote_mask();
    if (!mask) {
        put_why(why, why_cap, "REMOTEOBJECT mask not set");
        return false;
    }
    TakeCtx c;
    c.pick = &pick;
    c.taken = taken;
    c.n_taken = n_taken;
    c.mask = mask;
    probe_with_aircraft(take_cb, &c);
    if (!c.found) {
        put_why(why, why_cap, "no free friendly AI aircraft to use as an avatar");
        return false;
    }
    out->obj = c.obj;
    out->seq = c.seq;
    out->set_bits = mask & ~c.orig_flags;
    JetControls as_taken;  // the AI's gun and fire triggers stay in its input array otherwise
    if (jet_takeover(c.obj, &as_taken)) {
        out->engines_taken = as_taken.engines;
        out->engine_count_taken = as_taken.engine_count;
    }
    AvatarInfo local;
    AvatarInfo& in = info ? *info : local;
    in = AvatarInfo{};
    avatar_logic::to_ascii(in.name, sizeof(in.name), c.name);
    avatar_logic::to_ascii(in.type, sizeof(in.type), c.type);
    in.tier = c.rank.tier;
    in.flight_no = c.rank.flight_no;
    in.dist = c.rank.dist;
    in.nearby = c.rank.nearby;
    std::memcpy(in.from, c.from, sizeof(in.from));
    in.moved = avatar_logic::dist3(in.from, pick.place);
    if (why && why_cap) {
        const bool flight = in.tier == AvatarTier::Wingman;
        std::snprintf(why, why_cap, "\"%s\" %s (was at (%.0f, %.0f, %.0f), moved %.0f m); %s%s%s", in.name,
                      in.type[0] ? in.type : "(type unknown)", in.from[0], in.from[1], in.from[2], in.moved,
                      flight ? "" : "fallback, no free plane in the player's flight: ", tier_name(in.tier),
                      flight || in.nearby ? "" : "; no friendly free within 20 km, taken from farther");
    }
    return true;
}

void avatar_write(const AvatarHandle& h, const replay_math::Kin& k) {
    if (!h.obj) return;
    const float v[3] = {float(k.v[0]), float(k.v[1]), float(k.v[2])};
    const float ang[3] = {float(k.a[0]), float(k.a[1]), float(k.a[2])};
    const float rates[3] = {float(k.w[0]), float(k.w[1]), float(k.w[2])};
    float m[9];
    build_matrix(k.a, m);
    obj_write(h.obj, kOffPos, k.p, 3);
    obj_write(h.obj, kOffVel, v, 3);
    obj_write(h.obj, kOffRates, rates, 3);
    obj_write(h.obj, kOffMatrix, m, 9);
    obj_write(h.obj, kOffAngles, ang, 3);
}

void avatar_zero_rates(const AvatarHandle& h) {
    if (!h.obj) return;
    const float zero[3] = {};
    obj_write(h.obj, kOffRates, zero, 3);
}

bool avatar_keep_remote(const AvatarHandle& h) {
    if (!h.obj) return false;
    uint32_t flags;
    obj_read(h.obj, kOffFlags, &flags, 1);
    if ((flags & h.set_bits) == h.set_bits) return false;
    flags |= h.set_bits;
    obj_write(h.obj, kOffFlags, &flags, 1);
    return true;
}

void avatar_release(AvatarHandle& h, bool live) {
    weapons_forget_avatar(h.obj);
    if (h.obj && live) {
        uint32_t flags;
        obj_read(h.obj, kOffFlags, &flags, 1);
        flags &= ~h.set_bits;  // clear only what we set
        obj_write(h.obj, kOffFlags, &flags, 1);
        // An owner's engines-off must not leave the AI gliding.
        if (h.engine_count_taken) jet_restore_engines(h.obj, h.engines_taken, h.engine_count_taken);
        jet_clear_triggers(h.obj);
    }
    h = AvatarHandle{};
}

void avatar_check_live(const AvatarHandle* h, int n, bool* live) {
    for (int i = 0; i < n; ++i) live[i] = false;
    LiveCtx c{h, n, live};
    probe_with_aircraft(live_cb, &c);
}

}  // namespace LazyWorks::SF2Patch::hooks
