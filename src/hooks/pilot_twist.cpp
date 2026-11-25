// Pilot Twist (Options > Controls, default Off): in the cockpit view, past about 60 degrees of head yaw the view turns
// further (torso twist) and the eye moves a few centimetres toward that side and up. Hooks ViewManager.dll's inside
// view at fixed RVAs. See lazy-sf2-recon: recon-notes §25.
#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>

#include "config.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/pilot_twist_math.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

namespace pt = pilot_twist;

constexpr DWORD kViewTimestamp = 0x51d21fa0, kViewImageSize = 0x3b000;
constexpr DWORD kUpdateRva = 0x3360;  // inside view per-frame update: thiscall(view, float dt)
constexpr DWORD kCameraRva = 0x3070;  // builds the camera matrix from the view angles: thiscall(view, const float* aircraft)
constexpr DWORD kEyeRva = 0x2ec0;     // eye position: thiscall(view, const float* aircraft, void* aircraft_iface)
constexpr DWORD kObjectManagerRva = 0x356ec;  // object manager pointer

// View object.
constexpr int kViewDef = 0x0c;   // view definition*
constexpr int kObjectId = 0x1c;  // viewed object id
constexpr int kMaxYaw = 0xc0;    // float, rad (current limits)
constexpr int kMinYaw = 0xcc;
constexpr int kYaw = 0xdc;       // float, rad: smoothed seat-relative yaw (pitch, yaw, roll at +0xd8)
constexpr int kTracker = 0xe4;   // non-zero while a head tracker drives the view
constexpr int kEye = 0x90;       // double[3], world
// View definition.
constexpr int kDefInsideView = 0x20;  // short
constexpr int kDefYawControl = 0x10;  // int, -1 = the view has no yaw control
constexpr int kDefFlags = 0x24;
constexpr uint32_t kFlagSnapView = 0x2, kFlagLimitYaw = 0x2000;
// Interfaces (vtable byte offsets / 4).
constexpr int kGetInterface = 0x40 / 4;  // object manager: (id, kind, void** out)
constexpr int kSeatKind = 0xb;
constexpr int kSeatMovement = 0x1c / 4;  // seat: (float min[3], float max[3]) tracker head movement limits

using Update_t = void(__fastcall*)(void* view, void* edx, float dt);
using Camera_t = void(__fastcall*)(void* view, void* edx, const float* aircraft);
using Eye_t = void(__fastcall*)(void* view, void* edx, const float* aircraft, void* iface);
Update_t o_Update = nullptr;
Camera_t o_Camera = nullptr;
Eye_t o_Eye = nullptr;

// Install state, under g_install_lock. g_view_dll and g_enabled are also read without it (fast path, detours).
SRWLOCK g_install_lock = SRWLOCK_INIT;
std::atomic<HMODULE> g_view_dll{nullptr};  // image our (created) hooks belong to
HMODULE g_bad_base = nullptr;              // image already found unusable (checked once)
std::atomic<bool> g_enabled{false};        // hooks enabled (Pilot Twist on)

// View state. The three detours run on the one thread that updates the views (the game thread); nothing else
// touches these while the hooks are enabled, so they are plain variables.
void* g_updating = nullptr;  // the inside view whose update is running, else nullptr
const void* g_last_view = nullptr;
pt::TwistState g_state;
LARGE_INTEGER g_last_tick{};

template <typename T>
T& at(void* p, int offset) {
    return *reinterpret_cast<T*>(static_cast<BYTE*>(p) + offset);
}

// An inside (cockpit) view the player turns (it has a yaw control: hat or tracker), not a fixed snap view.
bool applies_to(void* view) {
    BYTE* def = at<BYTE*>(view, kViewDef);
    return def && at<short>(def, kDefInsideView) != 0 && (at<uint32_t>(def, kDefFlags) & kFlagSnapView) == 0 &&
           at<int>(def, kDefYawControl) != -1;
}

float seconds_since_last() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = g_state.have ? static_cast<float>(now.QuadPart - g_last_tick.QuadPart) / freq.QuadPart : 0.0f;
    g_last_tick = now;
    return dt;
}

void __fastcall h_Update(void* view, void* edx, float dt) {
    void* const outer = g_updating;  // survives a nested update
    g_updating = applies_to(view) ? view : nullptr;
    o_Update(view, edx, dt);
    g_updating = outer;
}

// Twist without feedback: the extra yaw is in the view's yaw only while the original builds this frame's camera
// matrix; then the head yaw is put back, so the game's smoothing and limits never see it.
void __fastcall h_Camera(void* view, void* edx, const float* aircraft) {
    if (!view || view != g_updating) {
        o_Camera(view, edx, aircraft);
        return;
    }
    if (view != g_last_view) g_state.have = false;  // another view: start at the target
    g_last_view = view;
    float& yaw = at<float>(view, kYaw);
    const float head = yaw;
    const float dt = seconds_since_last();
    BYTE* def = at<BYTE*>(view, kViewDef);
    const bool limited = (at<uint32_t>(def, kDefFlags) & kFlagLimitYaw) != 0;
    const float written = pt::camera_yaw(g_state, head, dt, at<float>(view, kMinYaw), at<float>(view, kMaxYaw), limited);
    yaw = written;
    o_Camera(view, edx, aircraft);
    yaw = pt::restore_yaw(yaw, written, head);
}

// Largest head movement the tracker may add (seat MinMovement/MaxMovement), 0 without a tracker.
float tracker_bound(void* view) {
    HMODULE vm = g_view_dll.load(std::memory_order_acquire);
    if (!at<int>(view, kTracker) || !vm) return 0.0f;
    void* om = *static_cast<void**>(at_rva(vm, kObjectManagerRva));
    if (!om) return 0.0f;
    void* seat = nullptr;
    using GetInterface_t = int(__fastcall*)(void*, void*, int, int, void**);
    reinterpret_cast<GetInterface_t>(vtable_slot(om, kGetInterface))(om, nullptr, at<int>(view, kObjectId), kSeatKind, &seat);
    if (!seat) return pt::kCapSide;  // unknown: leave no room for the lean rather than guess
    float lo[3] = {}, hi[3] = {};
    using Movement_t = int(__fastcall*)(void*, void*, float*, float*);
    reinterpret_cast<Movement_t>(vtable_slot(seat, kSeatMovement))(seat, nullptr, lo, hi);
    float b = 0.0f;
    for (float v : {lo[0], lo[2], hi[0], hi[2]}) b = std::fabs(v) > b ? std::fabs(v) : b;
    return b;
}

void __fastcall h_Eye(void* view, void* edx, const float* aircraft, void* iface) {
    o_Eye(view, edx, aircraft, iface);
    if (!view || view != g_updating || view != g_last_view || !aircraft || !g_state.have || g_state.weight == 0.0f) return;
    const pt::Vec3 d = pt::body_to_world(aircraft, pt::lean_body(g_state.weight, tracker_bound(view)));
    at<double>(view, kEye) += d.x;
    at<double>(view, kEye + 8) += d.y;
    at<double>(view, kEye + 16) += d.z;
}

constexpr DWORD kRvas[] = {kUpdateRva, kCameraRva, kEyeRva};

void remove_view_hooks(HMODULE base) {
    for (DWORD rva : kRvas) {
        MH_STATUS s = MH_RemoveHook(at_rva(base, rva));
        (void)s;  // MH_ERROR_NOT_CREATED is expected
    }
}

// True if base is still the loaded ViewManager.dll image (not freed). The build was checked when it was hooked.
bool still_loaded(HMODULE base) {
    HMODULE h = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(base), &h) &&
           h == base && GetModuleHandleW(L"ViewManager.dll") == base;
}

// Creates the three hooks on vm, disabled. False (and nothing left behind) if any fails.
bool create_view_hooks(HMODULE vm) {
    remove_view_hooks(vm);  // entries left by an earlier image at this base
    MH_STATUS s = MH_CreateHook(at_rva(vm, kUpdateRva), reinterpret_cast<void*>(&h_Update),
                                reinterpret_cast<void**>(&o_Update));
    if (s == MH_OK) {
        s = MH_CreateHook(at_rva(vm, kCameraRva), reinterpret_cast<void*>(&h_Camera), reinterpret_cast<void**>(&o_Camera));
    }
    if (s == MH_OK) s = MH_CreateHook(at_rva(vm, kEyeRva), reinterpret_cast<void*>(&h_Eye), reinterpret_cast<void**>(&o_Eye));
    LOG("pilot twist: hooks at ViewManager.dll+0x3360/+0x3070/+0x2ec0 created (disabled): %s", MH_StatusToString(s));
    if (s != MH_OK) remove_view_hooks(vm);
    return s == MH_OK;
}

// Enables or disables the three hooks together. MinHook's queued apply suspends the other threads once, patches the
// three entries in one go and moves any thread stopped inside a patched prologue, so no frame sees only some of
// them; a detour that is already running keeps its trampoline and finishes normally. Caller holds g_install_lock.
void set_enabled_locked(bool on) {
    HMODULE vm = g_view_dll.load();
    if (!vm || on == g_enabled.load()) return;
    for (DWORD rva : kRvas) {
        MH_STATUS q = on ? MH_QueueEnableHook(at_rva(vm, rva)) : MH_QueueDisableHook(at_rva(vm, rva));
        (void)q;  // reported by MH_ApplyQueued
    }
    const MH_STATUS s = MH_ApplyQueued();
    if (s == MH_OK) g_enabled = on;
    LOG("pilot twist: %s (view hooks %s: %s)", on ? "on" : "off", on ? "enabled" : "disabled", MH_StatusToString(s));
}

}  // namespace

// Module watch (maybe under the loader lock): reads only the setting already loaded (peek), never loads it.
void pilot_twist_on_modules_loaded() {
    HMODULE vm = GetModuleHandleW(L"ViewManager.dll");
    if (!vm) return;
    const auto* entry = static_cast<const BYTE*>(at_rva(vm, kUpdateRva));
    const bool want = config::pilot_twist_peek() == 1;
    // Fast path: our hooks belong to this image (an enabled one shows its JMP) and match the setting.
    if (vm == g_view_dll.load() && (!g_enabled.load() || *entry == 0xE9) && want == g_enabled.load()) return;

    AcquireSRWLockExclusive(&g_install_lock);
    do {
        if (vm == g_bad_base) break;
        HMODULE old = g_view_dll.load();
        // An enabled hook without its JMP: the DLL was freed and loaded again at the same base.
        const bool reloaded_here = vm == old && g_enabled.load() && *entry != 0xE9;
        if (vm != old || reloaded_here) {
            const bool build_ok = is_expected_build(vm, kViewTimestamp, kViewImageSize, "ViewManager.dll");
            if (old) {
                // MinHook restores the original bytes on removal: only while the old image is still this verified DLL.
                if (build_ok && still_loaded(old)) remove_view_hooks(old);
                g_view_dll = nullptr;
                g_enabled = false;
            }
            if (!build_ok || !create_view_hooks(vm)) {
                g_bad_base = vm;
                LOG("pilot twist: unavailable (%s)", build_ok ? "hooks not created" : "ViewManager.dll not recognised");
                break;
            }
            g_view_dll = vm;
        }
        set_enabled_locked(want);
    } while (false);
    ReleaseSRWLockExclusive(&g_install_lock);
}

// Called when Options > Controls is left (game thread, in the menus, so no view update is running).
void pilot_twist_apply_setting() {
    AcquireSRWLockExclusive(&g_install_lock);
    set_enabled_locked(config::pilot_twist());
    ReleaseSRWLockExclusive(&g_install_lock);
}

}  // namespace LazyWorks::SF2Patch::hooks
