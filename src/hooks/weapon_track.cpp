// Weapon tracker: follows missiles, bombs and rockets in flight for the Tacview recorder. Same enable rule and
// reload handling as aircraft_track.cpp. Fixed RVAs, July 2013 build only.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "config.h"
#include "hooks/aircraft_side.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/weapon_class.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr DWORD kMissileTimestamp = 0x51d220d1, kMissileImageSize = 0x46000;
constexpr DWORD kReleaseRva = 0x50e0;  // MissileObject release call
constexpr DWORD kDtorRva = 0xa7f0;     // MissileObject scalar deleting dtor
// Flag-mask globals (FRIENDLY, ENEMY; no NEUTRAL in this module).
constexpr DWORD kFriendlyMaskRva = 0x40588;
constexpr DWORD kEnemyMaskRva = 0x4058c;
constexpr size_t kInterfaceDelta = 0x14;  // the release call's this is the object + 0x14
constexpr size_t kFlagsOffset = 0x0c;
constexpr size_t kParentOffset = 0x100;  // pointer to the shooter's object, set before the release
constexpr size_t kDataOffset = 0x14c;    // pointer to the weapon data
constexpr size_t kDataTypeOffset = 0x298;  // int, the ini's WeaponDataType
constexpr size_t kMaxTracked = 256;
constexpr size_t kNameChars = 63;

struct Entry {
    void* obj;
    uint64_t seq;
    uint64_t parent_seq;
    uint8_t weapon_class;
    char name[kNameChars + 1];
};

SRWLOCK g_lock = SRWLOCK_INIT;
Entry g_table[kMaxTracked];  // guarded by g_lock
size_t g_count = 0;          // guarded by g_lock
uint64_t g_next_seq = 1;     // guarded by g_lock
std::atomic<uint32_t> g_skipped{0};       // weapons not tracked because the table was full, this flight
std::atomic<bool> g_skip_logged{false};

SRWLOCK g_install_lock = SRWLOCK_INIT;
std::atomic<uintptr_t> g_base{0};       // the MissileObject.dll image our hooks are on, or 0
std::atomic<bool> g_for_probe{false};
std::atomic<uintptr_t> g_skip_base{0};  // the image we looked at and did not hook (nothing needs it), or 0
std::atomic<uintptr_t> g_bad_base{0};   // an image of an unexpected build

using Release_t = void(__fastcall*)(void* self, void* edx, int* attached);
using Dtor_t = void*(__fastcall*)(void* self, void* edx, uint32_t flags);
Release_t o_Release = nullptr;
Dtor_t o_Dtor = nullptr;

// Reads what a released weapon needs from the object. No C++ objects (SEH). False if memory was unreadable.
struct WeaponInfo {
    const void* parent = nullptr;
    int32_t data_type = 0;
    char name[kNameChars + 1] = {};
};
bool read_weapon_info(const void* obj, WeaponInfo* out) {
    __try {
        const char* b = static_cast<const char*>(obj);
        out->parent = *reinterpret_cast<const void* const*>(b + kParentOffset);
        const char* data = *reinterpret_cast<const char* const*>(b + kDataOffset);
        if (!data) return false;  // Init did not find the weapon data: not a usable weapon
        out->data_type = *reinterpret_cast<const int32_t*>(data + kDataTypeOffset);
        // The data starts with the type name as a UTF-16 string of at most 64 characters.
        const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
        size_t k = 0;
        for (; k < kNameChars && w[k]; ++k) out->name[k] = (w[k] >= 0x20 && w[k] < 0x7f) ? static_cast<char>(w[k]) : '?';
        out->name[k] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safe_read_u32(const void* p, uint32_t* out) {
    __try {
        *out = *static_cast<const volatile uint32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Track only releases with no attached station, so pylon stores never enter the table.
void __fastcall h_Release(void* self, void* edx, int* attached) {
    o_Release(self, edx, attached);
    if (attached) return;  // attached to a station: carried, not in flight
    void* obj = static_cast<char*>(self) - kInterfaceDelta;
    weapons_on_release(obj);  // the network layer (weapon_net.cpp); inert outside a shared-world session
    WeaponInfo info;
    if (!read_weapon_info(obj, &info)) return;
    // Before taking our lock: the aircraft table has its own, and nothing may hold both in the other order.
    const uint64_t parent_seq = aircraft_seq_for_object(info.parent);
    const uint8_t cls = static_cast<uint8_t>(classify_weapon_type(info.data_type));
    bool full = false;
    uint64_t seq = 0;
    size_t live = 0;
    AcquireSRWLockExclusive(&g_lock);
    size_t slot = g_count;
    for (size_t i = 0; i < g_count; ++i) {
        if (g_table[i].obj == obj) {  // released again, or an address reused without a destructor call we saw
            slot = i;
            break;
        }
    }
    if (slot < kMaxTracked) {
        Entry& e = g_table[slot];
        e.obj = obj;
        e.seq = seq = g_next_seq++;
        e.parent_seq = parent_seq;
        e.weapon_class = cls;
        std::memcpy(e.name, info.name, sizeof(e.name));
        if (slot == g_count) ++g_count;
    } else {
        full = true;
    }
    live = g_count;
    ReleaseSRWLockExclusive(&g_lock);
    if (full) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        if (!g_skip_logged.exchange(true))
            LOG("weapons: table full (%zu); further weapons are not tracked this flight", kMaxTracked);
    } else if (g_for_probe.load()) {
        LOG("weapons: + %p \"%s\" type %d class %u seq %llu parent seq %llu (%zu live)", obj, info.name, info.data_type,
            static_cast<unsigned>(cls), static_cast<unsigned long long>(seq),
            static_cast<unsigned long long>(parent_seq), live);
    }
}

// Remove the entry before the original runs, so the sampler (shared lock) never sees an object being freed.
void* __fastcall h_Dtor(void* self, void* edx, uint32_t flags) {
    size_t live = 0;
    bool removed = false;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        if (g_table[i].obj == self) {
            removed = true;
            g_table[i] = g_table[--g_count];
            break;
        }
    }
    live = g_count;
    ReleaseSRWLockExclusive(&g_lock);
    if (removed && g_for_probe.load()) LOG("weapons: - %p (%zu live)", self, live);
    return o_Dtor(self, edx, flags);
}

void clear_table() {
    AcquireSRWLockExclusive(&g_lock);
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void remove_weapon_hooks(HMODULE base) {
    for (DWORD rva : {kReleaseRva, kDtorRva}) {
        MH_STATUS s = MH_RemoveHook(at_rva(base, rva));
        (void)s;  // MH_ERROR_NOT_CREATED is expected
    }
}

// True if base is still the loaded MissileObject.dll image (not freed, not replaced by another DLL).
bool old_image_still_loaded(HMODULE base) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(base), &h) ||
        h != base) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(base, path, MAX_PATH);
    static constexpr wchar_t kName[] = L"\\MissileObject.dll";
    constexpr DWORD kLen = static_cast<DWORD>(_countof(kName) - 1);
    if (n >= MAX_PATH || n < kLen) return false;
    return CompareStringOrdinal(path + n - kLen, static_cast<int>(kLen), kName, static_cast<int>(kLen), TRUE) ==
           CSTR_EQUAL;
}

bool hooks_carry_patch(HMODULE m) {
    return *static_cast<const BYTE*>(at_rva(m, kReleaseRva)) == 0xE9;  // MinHook writes a JMP rel32
}

// MissileObject's ctor is hooked by missile_spawn.cpp, not here (one hook per address).
bool install_for(HMODULE m) {
    remove_weapon_hooks(m);  // stale entries of an earlier load at this base
    clear_table();
    bool ok = create_and_enable(at_rva(m, kReleaseRva), reinterpret_cast<void*>(&h_Release),
                                reinterpret_cast<void**>(&o_Release), "MissileObject release");
    ok = ok && create_and_enable(at_rva(m, kDtorRva), reinterpret_cast<void*>(&h_Dtor),
                                 reinterpret_cast<void**>(&o_Dtor), "MissileObject dtor");
    if (!ok) remove_weapon_hooks(m);
    return ok;
}

bool tacview_setting_peek() { return config::tacview_recording_peek() == 1; }

void install_and_publish(HMODULE m, bool probe, bool tacview) {
    const auto mu = reinterpret_cast<uintptr_t>(m);
    g_skip_base = 0;
    g_for_probe = probe;
    if (install_for(m)) {
        g_base.store(mu, std::memory_order_release);
        LOG("weapons: tracking on (reason: %s%s%s)", probe ? "probe" : "", probe && tacview ? " and " : "",
            tacview ? "Tacview Recording setting" : "");
    } else {
        g_for_probe = false;
        g_skip_base = mu;
        LOG("weapons: installing the weapon hooks failed; weapons are not recorded");
    }
}

}  // namespace

bool weapon_track_active() { return g_base.load(std::memory_order_acquire) != 0; }

void weapon_track_on_modules_loaded() {
    HMODULE m = GetModuleHandleW(L"MissileObject.dll");
    if (!m) return;
    const auto mu = reinterpret_cast<uintptr_t>(m);
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
                      is_expected_build(m, kMissileTimestamp, kMissileImageSize, "MissileObject.dll");
        in.wanted = tracking_wanted(probe, tacview);
        in.had_old = old != 0;
        const LoadDecision d = decide_on_image(in);
        if (d.kind == LoadKind::Keep) break;
        if (d.kind == LoadKind::BadBuild) {
            g_bad_base = mu;
            g_base = 0;
            g_for_probe = false;
            break;
        }
        if (in.had_old) {
            g_base = 0;
            g_for_probe = false;
            if (d.drop_old) {
                if (old_image_still_loaded(reinterpret_cast<HMODULE>(old))) {
                    remove_weapon_hooks(reinterpret_cast<HMODULE>(old));
                } else {
                    LOG("weapons: old MissileObject.dll image gone, dropping its hooks without restoring");
                }
            }
            clear_table();  // its objects went with the image
        }
        if (d.kind == LoadKind::Skip) {
            g_skip_base = mu;
            LOG("weapons: not needed (probe off, Tacview Recording off); MissileObject.dll left unhooked");
            break;
        }
        install_and_publish(m, probe, tacview);
    } while (false);
    ReleaseSRWLockExclusive(&g_install_lock);
}

void weapon_track_reevaluate() {
    HMODULE m = GetModuleHandleW(L"MissileObject.dll");
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
                if (!is_expected_build(m, kMissileTimestamp, kMissileImageSize, "MissileObject.dll")) {
                    g_bad_base = mu;
                    break;
                }
                install_and_publish(m, probe, tacview);
                break;
            case OptionsExitAction::Remove:
                g_base = 0;
                g_for_probe = false;
                g_skip_base = mu;
                if (old_image_still_loaded(m)) remove_weapon_hooks(m);
                clear_table();
                LOG("weapons: no longer needed (Tacview Recording off, probe off); weapon hooks removed");
                break;
        }
    } while (false);
    ReleaseSRWLockExclusive(&g_install_lock);
}

size_t weapon_track_drop_all() {
    AcquireSRWLockExclusive(&g_lock);
    const size_t dropped = g_count;
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return dropped;
}

void weapon_track_new_flight() {
    const uint32_t skipped = g_skipped.exchange(0);
    g_skip_logged = false;
    if (skipped) LOG("weapons: %u weapons were not tracked in the last flight (table full)", skipped);
}

void probe_with_weapons(void (*callback)(const LiveWeapon* list, size_t count, void* ctx), void* ctx) {
    LiveWeapon list[kMaxTracked];  // a copy, so the callback never sees Entry
    const uintptr_t base = g_base.load(std::memory_order_acquire);
    uint32_t friendly = 0, enemy = 0;
    if (base) {
        safe_read_u32(reinterpret_cast<const void*>(base + kFriendlyMaskRva), &friendly);
        safe_read_u32(reinterpret_cast<const void*>(base + kEnemyMaskRva), &enemy);
    }
    AcquireSRWLockShared(&g_lock);
    const size_t n = g_count;
    for (size_t i = 0; i < n; ++i) {
        const Entry& e = g_table[i];
        uint32_t flags = 0;
        const bool have = safe_read_u32(static_cast<const BYTE*>(e.obj) + kFlagsOffset, &flags);
        list[i] = {e.obj, e.name, e.seq, e.weapon_class, e.parent_seq,
                   have ? classify_side(flags, friendly, enemy, 0) : AircraftSide::Unknown};
    }
    callback(list, n, ctx);  // the callback must not log or touch files
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace LazyWorks::SF2Patch::hooks
