// Aircraft tracker: the production base of the object probe and of the Tacview recorder. Follows the live
// aircraft objects (constructor and destructor hooks) in a small table and, through sim_hooks.cpp, the
// per-step Update hook and game clock.
//
// Nothing is hooked unless a client needs it: the probe is on (LAZYSF2_PROBE_DIR) or the Tacview Recording
// setting is on. That is decided only when no aircraft can exist: when a new AircraftObject.dll image appears
// (module watch) and when an Options page is left (aircraft_track_reevaluate). Never mid-flight: with aircraft
// already live, an empty table would make an AI aircraft the player. The module watch may run under the loader
// lock, so it only peeks at the setting (config::tacview_recording_peek) and never reads a file.
//
// The game can free and reload AircraftObject.dll (possibly at the same address). As the menu hooks do for
// OptionsScreen.dll, we re-hook whenever the loaded image does not carry our patch, remove the previous image's
// hooks only while it is still the verified DLL, and remove stale MinHook entries at the new base first.
//
// The player rule: the player is the first aircraft constructed since the table was last empty.
// Hooks are at fixed RVAs and only installed on the July 2013 build.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "config.h"
#include "hooks/aircraft_side.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

// Bumped whenever a tracked aircraft is destroyed or its address is reused, and when the tracker re-hooks (see hooks.h).
std::atomic<uint32_t> g_aircraft_generation{0};

namespace {

constexpr DWORD kAircraftTimestamp = 0x51d220bd, kAircraftImageSize = 0xc2000;
constexpr DWORD kCtorRva = 0x48cf0;  // AircraftObjectClass* __thiscall ctor(this, const wchar_t* name)  (ret 4)
constexpr DWORD kDtorRva = 0x41600;  // void __thiscall dtor(this)
// Module globals holding the FRIENDLY, ENEMY and NEUTRAL flag masks: the side bits of an object's flag word
// (object +0x0c).
constexpr DWORD kFriendlyMaskRva = 0xac460;
constexpr DWORD kEnemyMaskRva = 0xac464;
constexpr DWORD kNeutralMaskRva = 0xac468;
constexpr size_t kFlagsOffset = 0x0c;
constexpr size_t kMaxTracked = 256;
constexpr size_t kNameChars = 63;
constexpr size_t kTypeChars = 31;
constexpr size_t kDataPtrOffset = 0x380;  // object -> pointer to the aircraft data record (set by the constructor)
constexpr size_t kDataTypeNameOffset = 0x04;  // record: type name, UTF-16, up to 64 characters

struct Entry {
    void* obj;
    wchar_t name[kNameChars + 1];
    char type[kTypeChars + 1];  // ASCII; empty when unknown
    ULONGLONG created;  // GetTickCount64(): coarse, ties: never identity
    uint64_t seq;       // construction sequence number, from 1; unique and increasing
};

SRWLOCK g_lock = SRWLOCK_INIT;
Entry g_table[kMaxTracked];  // guarded by g_lock
size_t g_count = 0;          // guarded by g_lock
bool g_full_logged = false;  // guarded by g_lock
uint64_t g_next_seq = 1;     // guarded by g_lock
// The player's seq, or 0 before any. Unchanged when that aircraft is destroyed (no entry matches then), so
// the player is unknown until the table empties and a new aircraft is constructed.
uint64_t g_player_seq = 0;   // guarded by g_lock

// Install state. Writers hold g_install_lock; the fast path reads the atomics without it.
SRWLOCK g_install_lock = SRWLOCK_INIT;
std::atomic<uintptr_t> g_base{0};       // the AircraftObject.dll image our hooks are on, or 0
std::atomic<bool> g_for_probe{false};
std::atomic<bool> g_for_tacview{false};
std::atomic<uintptr_t> g_skip_base{0};  // the image we looked at and did not hook (nothing needs it), or 0
std::atomic<uintptr_t> g_bad_base{0};   // an image of an unexpected build

using Ctor_t = void*(__fastcall*)(void* self, void* edx, const wchar_t* name);
using Dtor_t = void(__fastcall*)(void* self, void* edx);
Ctor_t o_Ctor = nullptr;
Dtor_t o_Dtor = nullptr;

// The aircraft type name from the object's data record, ASCII-narrowed. Empty on any failure. No C++ objects (SEH).
void read_type_name(const void* obj, char* out) {
    out[0] = '\0';
    __try {
        const char* rec = *reinterpret_cast<const char* const*>(static_cast<const char*>(obj) + kDataPtrOffset);
        if (!rec) return;
        const wchar_t* w = reinterpret_cast<const wchar_t*>(rec + kDataTypeNameOffset);
        size_t k = 0;
        for (; k < kTypeChars && w[k]; ++k) out[k] = (w[k] >= 0x20 && w[k] < 0x7f) ? static_cast<char>(w[k]) : '?';
        out[k] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

void* __fastcall h_Ctor(void* self, void* edx, const wchar_t* name) {
    void* r = o_Ctor(self, edx, name);
    char type[kTypeChars + 1];
    read_type_name(self, type);  // before the lock: the constructor has set the data record pointer
    bool full = false, became_player = false;
    uint64_t seq = 0;
    wchar_t shown[kNameChars + 1] = {};  // the truncated copy, for the log line
    AcquireSRWLockExclusive(&g_lock);
    size_t slot = g_count;
    const bool table_empty = g_count == 0;
    for (size_t i = 0; i < g_count; ++i) {
        if (g_table[i].obj == self) {  // address reused without a destructor call we saw
            slot = i;
            g_aircraft_generation.fetch_add(1, std::memory_order_release);
            break;
        }
    }
    if (slot < kMaxTracked) {
        Entry& e = g_table[slot];
        e.obj = self;
        e.name[0] = L'\0';
        if (name) {
            // The game's string may be anything; copy at most kNameChars characters.
            for (size_t i = 0; i < kNameChars && name[i]; ++i) {
                e.name[i] = name[i];
                e.name[i + 1] = L'\0';
            }
        }
        std::memcpy(e.type, type, sizeof(e.type));
        e.created = GetTickCount64();
        e.seq = seq = g_next_seq++;
        if (table_empty) {
            g_player_seq = seq;
            became_player = true;
        }
        wcscpy_s(shown, e.name);
        if (slot == g_count) ++g_count;
    } else if (!g_full_logged) {
        g_full_logged = full = true;
    }
    const size_t live = g_count;
    ReleaseSRWLockExclusive(&g_lock);
    // Per-aircraft lines only in probe mode; a player build logs the player, a full table and the table emptying.
    if (full) LOG("track: aircraft table full (%zu); further aircraft are not tracked", kMaxTracked);
    if (g_for_probe.load()) {
        LOG("track: aircraft + %p \"%ls\" type \"%s\" seq %llu (%zu live)", self, shown, type,
            static_cast<unsigned long long>(seq), live);
    }
    if (became_player) {
        LOG("track: player chosen: %p \"%ls\" seq %llu (first constructed since the table was empty)", self, shown,
            static_cast<unsigned long long>(seq));
    }
    return r;
}

void __fastcall h_Dtor(void* self, void* edx) {
    // Remove first, so a sampler (which holds the lock while it reads) never sees an object whose
    // destructor has started, and the object is never freed while listed.
    bool removed = false;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        if (g_table[i].obj == self) {
            removed = true;
            g_table[i] = g_table[--g_count];
            g_aircraft_generation.fetch_add(1, std::memory_order_release);
            break;
        }
    }
    const size_t live = g_count;
    ReleaseSRWLockExclusive(&g_lock);
    if (g_for_probe.load()) {
        LOG("track: aircraft - %p (%zu live)", self, live);
    }
    if (live == 0 && removed) {
        const size_t weapons_dropped = weapon_track_drop_all();  // the flight is over; no stale weapons into the next
        if (!g_for_probe.load())
            LOG("track: table empty (the flight's aircraft are gone; %zu live weapons dropped)", weapons_dropped);
        tacview_recorder_table_empty();
    }
    o_Dtor(self, edx);
}

// Reads 4 bytes that may have gone away (an image freed under us); false instead of a crash.
bool safe_read_u32(const void* p, uint32_t* out) {
    __try {
        *out = *static_cast<const volatile uint32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The side masks of the hooked image; 0 for a mask that cannot be read.
struct SideMasks {
    uint32_t friendly = 0, enemy = 0, neutral = 0;
};
SideMasks read_side_masks() {
    SideMasks m;
    const uintptr_t base = g_base.load(std::memory_order_acquire);
    if (!base) return m;
    safe_read_u32(reinterpret_cast<const void*>(base + kFriendlyMaskRva), &m.friendly);
    safe_read_u32(reinterpret_cast<const void*>(base + kEnemyMaskRva), &m.enemy);
    safe_read_u32(reinterpret_cast<const void*>(base + kNeutralMaskRva), &m.neutral);
    return m;
}

void clear_table() {
    AcquireSRWLockExclusive(&g_lock);
    g_count = 0;
    g_player_seq = 0;
    g_full_logged = false;
    g_aircraft_generation.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
}

void remove_track_hooks(HMODULE base) {
    for (DWORD rva : {kCtorRva, kDtorRva}) {
        MH_STATUS s = MH_RemoveHook(at_rva(base, rva));
        (void)s;  // MH_ERROR_NOT_CREATED is expected
    }
    sim_remove(base);
}

// True if base is still the loaded AircraftObject.dll image (not freed, not replaced by another DLL).
bool old_image_still_loaded(HMODULE base) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(base), &h) ||
        h != base) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(base, path, MAX_PATH);
    static constexpr wchar_t kName[] = L"\\AircraftObject.dll";
    constexpr DWORD kLen = static_cast<DWORD>(_countof(kName) - 1);
    if (n >= MAX_PATH || n < kLen) return false;  // truncated: the name can't be verified, so don't touch it
    return CompareStringOrdinal(path + n - kLen, static_cast<int>(kLen), kName, static_cast<int>(kLen), TRUE) ==
           CSTR_EQUAL;
}

bool hooks_carry_patch(HMODULE m) {
    const auto* ctor = static_cast<const BYTE*>(at_rva(m, kCtorRva));
    return *ctor == 0xE9;  // MinHook writes a JMP rel32
}

// Called with g_install_lock held, for an image m of the expected build that needs tracking.
bool install_for(HMODULE m) {
    // A previous load at this base may have left entries in MinHook (its image was freed while they were
    // registered); remove them so MH_CreateHook doesn't fail with ALREADY_CREATED. Safe: m is a verified image.
    remove_track_hooks(m);
    clear_table();  // a new image, or the menus: no live aircraft; start from an empty table
    bool ok = create_and_enable(at_rva(m, kCtorRva), reinterpret_cast<void*>(&h_Ctor),
                                reinterpret_cast<void**>(&o_Ctor), "AircraftObject ctor");
    ok = ok && create_and_enable(at_rva(m, kDtorRva), reinterpret_cast<void*>(&h_Dtor),
                                 reinterpret_cast<void**>(&o_Dtor), "AircraftObject dtor");
    ok = ok && sim_install(m);
    if (!ok) remove_track_hooks(m);
    return ok;
}

// The Tacview Recording setting as the module watch may see it: the loaded value only, never loads anything.
bool tacview_setting_peek() {
    const int v = config::tacview_recording_peek();
    if (v < 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) LOG("track: settings not loaded yet; Tacview Recording treated as off");
        return false;
    }
    return v == 1;
}

// Hooks m (verified build) for the given clients and publishes the state. g_install_lock held.
void install_and_publish(HMODULE m, bool probe, bool tacview) {
    const auto mu = reinterpret_cast<uintptr_t>(m);
    g_skip_base = 0;
    g_for_probe = probe;  // before the hooks can run: they gate their log lines on it
    g_for_tacview = tacview;
    if (install_for(m)) {
        g_base.store(mu, std::memory_order_release);
        LOG("track: aircraft tracking on (reason: %s%s%s)", probe ? "probe" : "", probe && tacview ? " and " : "",
            tacview ? "Tacview Recording setting" : "");
    } else {
        g_for_probe = g_for_tacview = false;
        g_skip_base = mu;  // don't retry at every module load
        LOG("track: installing the aircraft hooks failed; tracking stays off");
    }
}

}  // namespace

bool aircraft_track_active() { return g_base.load(std::memory_order_acquire) != 0; }
bool aircraft_track_for_probe() { return g_for_probe.load(); }
bool aircraft_track_for_tacview() { return g_for_tacview.load(); }

void aircraft_track_on_modules_loaded() {
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (!m) return;
    const auto mu = reinterpret_cast<uintptr_t>(m);
    // Fast paths, with no lock: already hooked, a known-bad build, or an image already decided on (not hooked).
    if (mu == g_base.load(std::memory_order_acquire) && hooks_carry_patch(m)) return;
    if (mu == g_bad_base.load() || mu == g_skip_base.load()) return;

    AcquireSRWLockExclusive(&g_install_lock);
    do {
        const uintptr_t old = g_base.load();
        const bool probe = probe_output_dir() != nullptr;
        const bool tacview = tacview_setting_peek();
        LoadInput in{};
        in.hooked_patched = mu == old && hooks_carry_patch(m);
        in.build_ok = in.hooked_patched ||
                      is_expected_build(m, kAircraftTimestamp, kAircraftImageSize, "AircraftObject.dll");
        in.wanted = tracking_wanted(probe, tacview);
        in.had_old = old != 0;
        const LoadDecision d = decide_on_image(in);
        if (d.kind == LoadKind::Keep) break;  // another thread got here first
        if (d.kind == LoadKind::BadBuild) {
            g_bad_base = mu;
            g_base = 0;
            g_for_probe = g_for_tacview = false;
            break;
        }
        if (in.had_old) {
            // Forget the hooks on the previous image. MinHook restores the original bytes on removal, so only do
            // that while the old base is still our AircraftObject.dll; otherwise those bytes would be written
            // into whatever is mapped there now.
            g_base = 0;
            g_for_probe = g_for_tacview = false;
            if (d.drop_old) {
                if (old_image_still_loaded(reinterpret_cast<HMODULE>(old))) {
                    remove_track_hooks(reinterpret_cast<HMODULE>(old));
                } else {
                    LOG("track: old AircraftObject.dll image gone, dropping its hooks without restoring");
                }
            }
        }
        if (d.kind == LoadKind::Skip) {
            g_skip_base = mu;
            LOG("track: not needed (probe off, Tacview Recording off); AircraftObject.dll left unhooked");
            break;
        }
        install_and_publish(m, probe, tacview);
    } while (false);
    ReleaseSRWLockExclusive(&g_install_lock);
}

void aircraft_track_reevaluate() {
    tacview_recorder_menu_moment();  // game thread, menus: closes the recording of a flight that has ended
    weapon_track_reevaluate();       // the weapon hooks follow the same rule, at the same moment
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    const auto mu = reinterpret_cast<uintptr_t>(m);
    const bool probe = probe_output_dir() != nullptr;
    const bool tacview = config::tacview_recording_peek() == 1;
    AcquireSRWLockExclusive(&g_install_lock);
    do {
        const bool hooked = m && mu == g_base.load() && hooks_carry_patch(m);
        const bool wanted = tracking_wanted(probe, tacview);
        switch (decide_on_options_exit(m != nullptr && mu != g_bad_base.load(), hooked, wanted)) {
            case OptionsExitAction::Nothing:
                break;
            case OptionsExitAction::Install:
                if (!is_expected_build(m, kAircraftTimestamp, kAircraftImageSize, "AircraftObject.dll")) {
                    g_bad_base = mu;
                    break;
                }
                install_and_publish(m, probe, tacview);
                break;
            case OptionsExitAction::Remove:
                // Same safety as at a reload: only touch the image while it is still our verified DLL.
                g_base = 0;
                g_for_probe = g_for_tacview = false;
                g_skip_base = mu;
                if (old_image_still_loaded(m)) remove_track_hooks(m);
                clear_table();
                LOG("track: no longer needed (Tacview Recording off, probe off); aircraft hooks removed");
                break;
        }
    } while (false);
    ReleaseSRWLockExclusive(&g_install_lock);
}

unsigned long long aircraft_seq_for_object(const void* obj) {
    if (!obj) return 0;
    unsigned long long seq = 0;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        if (g_table[i].obj == obj) {
            seq = g_table[i].seq;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return seq;
}

void probe_with_aircraft(void (*callback)(const LiveAircraft* list, size_t count, void* ctx), void* ctx) {
    LiveAircraft list[kMaxTracked];  // a copy of the table, so the callback never sees Entry
    const SideMasks masks = read_side_masks();
    AcquireSRWLockShared(&g_lock);
    const size_t n = g_count;
    for (size_t i = 0; i < n; ++i) {
        const Entry& e = g_table[i];
        uint32_t flags = 0;
        const bool have = safe_read_u32(static_cast<const BYTE*>(e.obj) + kFlagsOffset, &flags);
        list[i] = {e.obj,
                   e.name,
                   e.seq,
                   e.seq == g_player_seq,
                   have ? classify_side(flags, masks.friendly, masks.enemy, masks.neutral) : AircraftSide::Unknown,
                   e.created,
                   e.type};
    }
    callback(list, n, ctx);  // the callback must not log or touch files
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace lazysf2::hooks
