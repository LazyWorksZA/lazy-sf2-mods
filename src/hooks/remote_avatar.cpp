// Remote avatars: taking over an AI aircraft, driving it, and giving it back. See remote_avatar.h.
#include "hooks/remote_avatar.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

using replay_math::build_matrix;

constexpr DWORD kAircraftTimestamp = 0x51d220bd, kAircraftImageSize = 0xc2000;
constexpr DWORD kRemoteMaskRva = 0xb7b28;  // AircraftObject.dll global: the REMOTEOBJECT flag mask
constexpr size_t kOffFlags = 0xc, kOffPos = 0x38, kOffVel = 0x50, kOffRates = 0xd0, kOffMatrix = 0xdc,
                 kOffAngles = 0x100;

template <typename T>
void read_at(const void* obj, size_t off, T* out, size_t n) {
    std::memcpy(out, static_cast<const char*>(obj) + off, n * sizeof(T));
}
template <typename T>
void write_at(void* obj, size_t off, const T* in, size_t n) {
    std::memcpy(static_cast<char*>(obj) + off, in, n * sizeof(T));
}
double dist3(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

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
    double ref[3];
    if (player) {
        read_at(player->obj, kOffPos, ref, 3);
    } else {
        std::memcpy(ref, c.pick->place, sizeof(ref));
    }
    AvatarPicker picker(player ? player->name : nullptr, c.pick->want_type);
    for (size_t i = 0; i < n; ++i) {
        bool taken = false;
        for (int t = 0; t < c.n_taken; ++t)
            if (c.taken[t].obj && c.taken[t].obj == list[i].obj && c.taken[t].seq == list[i].seq) taken = true;
        double p[3];
        read_at(list[i].obj, kOffPos, p, 3);
        const AvatarCandidate cand{list[i].name, list[i].type_name, list[i].seq, list[i].side, list[i].is_player, taken,
                                   dist3(p, ref)};
        picker.offer(i, cand);
    }
    if (!picker.found()) return;
    const LiveAircraft& a = list[picker.index()];
    c.found = true;
    c.obj = a.obj;
    c.seq = a.seq;
    c.rank = picker.best();
    read_at(c.obj, kOffPos, c.from, 3);
    if (a.name) wcsncpy_s(c.name, a.name, _TRUNCATE);
    if (a.type_name) strncpy_s(c.type, a.type_name, _TRUNCATE);
    // REMOTEOBJECT on, rates zero, at the place: the aircraft coasts and we are the only writer. Done under the lock,
    // so the aircraft cannot be destroyed between the choice and the writes.
    char* obj = static_cast<char*>(c.obj);
    read_at(obj, kOffFlags, &c.orig_flags, 1);
    const float zero[3] = {};
    write_at(obj, kOffRates, zero, 3);
    write_at(obj, kOffVel, c.pick->vel, 3);
    const uint32_t flags = c.orig_flags | c.mask;
    write_at(obj, kOffFlags, &flags, 1);
    write_at(obj, kOffPos, c.pick->place, 3);
    const double ang[3] = {0.0, static_cast<double>(c.pick->heading), 0.0};
    const float angf[3] = {0.0f, c.pick->heading, 0.0f};
    float m[9];
    build_matrix(ang, m);
    write_at(obj, kOffMatrix, m, 9);
    write_at(obj, kOffAngles, angf, 3);
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
            if (c.h[s].obj && list[i].obj == c.h[s].obj && list[i].seq == c.h[s].seq) c.live[s] = true;
}

// Printable ASCII only, for log lines.
void to_ascii(char* dst, size_t cap, const wchar_t* src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = (src[i] >= 0x20 && src[i] <= 0x7e) ? static_cast<char>(src[i]) : '?';
    dst[i] = '\0';
}
void ascii_copy(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = (src[i] >= 0x20 && src[i] <= 0x7e) ? src[i] : '?';
    dst[i] = '\0';
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
        put_why(why, why_cap, "no free AI aircraft to use as an avatar");
        return false;
    }
    out->obj = c.obj;
    out->seq = c.seq;
    out->set_bits = mask & ~c.orig_flags;
    AvatarInfo local;
    AvatarInfo& in = info ? *info : local;
    in = AvatarInfo{};
    to_ascii(in.name, sizeof(in.name), c.name);
    ascii_copy(in.type, sizeof(in.type), c.type);
    in.tier = c.rank.tier;
    in.flight_no = c.rank.flight_no;
    in.dist = c.rank.dist;
    std::memcpy(in.from, c.from, sizeof(in.from));
    if (why && why_cap) {
        std::snprintf(why, why_cap, "\"%s\" %s (was %.0f m from the player at (%.0f, %.0f, %.0f)); %s", in.name,
                      in.type[0] ? in.type : "(type unknown)", in.dist, in.from[0], in.from[1], in.from[2],
                      tier_name(in.tier));
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
    write_at(h.obj, kOffPos, k.p, 3);
    write_at(h.obj, kOffVel, v, 3);
    write_at(h.obj, kOffRates, rates, 3);
    write_at(h.obj, kOffMatrix, m, 9);
    write_at(h.obj, kOffAngles, ang, 3);
}

bool avatar_keep_remote(const AvatarHandle& h) {
    if (!h.obj) return false;
    uint32_t flags;
    read_at(h.obj, kOffFlags, &flags, 1);
    if ((flags & h.set_bits) == h.set_bits) return false;
    flags |= h.set_bits;
    write_at(h.obj, kOffFlags, &flags, 1);
    return true;
}

void avatar_release(AvatarHandle& h, bool live) {
    if (h.obj && live) {
        uint32_t flags;
        read_at(h.obj, kOffFlags, &flags, 1);
        flags &= ~h.set_bits;  // clear only what we set
        write_at(h.obj, kOffFlags, &flags, 1);
    }
    h = AvatarHandle{};
}

void avatar_check_live(const AvatarHandle* h, int n, bool* live) {
    for (int i = 0; i < n; ++i) live[i] = false;
    LiveCtx c{h, n, live};
    probe_with_aircraft(live_cb, &c);
}

}  // namespace LazyWorks::SF2Patch::hooks
