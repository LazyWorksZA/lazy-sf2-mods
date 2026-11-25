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
constexpr DWORD kInputManagerRva = 0x356e8;   // input manager pointer (diagnostics: tracker axes)

// View object.
constexpr int kViewDef = 0x0c;   // view definition*
constexpr int kObjectId = 0x1c;  // viewed object id
constexpr int kMaxYaw = 0xc0;    // float, rad (current limits)
constexpr int kMinYaw = 0xcc;
constexpr int kYaw = 0xdc;       // float, rad: smoothed seat-relative yaw (pitch, yaw, roll at +0xd8)
constexpr int kTargetYaw = 0xac;  // float, rad: yaw target (hat-integrated, or the tracker yaw in tracker mode)
constexpr int kTracker = 0xe4;   // non-zero while a head tracker drives the view
constexpr int kTrackerMode = 0xe8;  // short: 1 while the tracker (not the hat) sets the angles
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
constexpr int kReadAxis = 0x8c / 4;      // input manager: int (int axis), tracker axes 0..4 = x, z, y, yaw, pitch

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
std::atomic<bool> g_reset{false};          // set on disable: the next camera build starts from a clean state

// View state. The three detours run on the one thread that updates the views (the game thread); nothing else
// touches these while the hooks are enabled, so they are plain variables.
void* g_updating = nullptr;  // the inside view whose update is running, else nullptr
const void* g_last_view = nullptr;
pt::TwistState g_state;
LARGE_INTEGER g_last_tick{};

// Developer diagnostics (LAZYSF2_PROBE_DIR set): one log line per second while an inside view updates.
struct Diag {
    bool camera, eye;
    float head, yaw, weight, delta, bound;
    double lean[3];
};
Diag g_diag{};
ULONGLONG g_diag_next = 0;

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

void diag_log(void* view);

void __fastcall h_Update(void* view, void* edx, float dt) {
    void* const outer = g_updating;  // survives a nested update
    g_updating = applies_to(view) ? view : nullptr;
    const bool probe = probe_output_dir() != nullptr;
    if (probe) g_diag = {};
    o_Update(view, edx, dt);
    g_updating = outer;
    if (probe) diag_log(view);
}

// Twist without feedback: the extra yaw is in the view's yaw only while the original builds this frame's camera
// matrix; then the head yaw is put back, so the game's smoothing and limits never see it.
void __fastcall h_Camera(void* view, void* edx, const float* aircraft) {
    if (!view || view != g_updating) {
        o_Camera(view, edx, aircraft);
        return;
    }
    if (g_reset.exchange(false)) g_last_view = nullptr;  // turned off and on again: forget the old filter and side
    if (view != g_last_view) g_state = {};  // another view: start at the target
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
    g_diag.camera = true;
    g_diag.head = head;
    g_diag.yaw = g_state.yaw;
    g_diag.weight = g_state.weight;
    g_diag.delta = written - head;
}

// The seat's tracker head movement limits (MinMovement/MaxMovement x and z); false if the seat can't be asked.
bool seat_movement(void* view, float lo[3], float hi[3]) {
    HMODULE vm = g_view_dll.load(std::memory_order_acquire);
    void* om = vm ? *static_cast<void**>(at_rva(vm, kObjectManagerRva)) : nullptr;
    if (!om) return false;
    void* seat = nullptr;
    using GetInterface_t = int(__fastcall*)(void*, void*, int, int, void**);
    reinterpret_cast<GetInterface_t>(vtable_slot(om, kGetInterface))(om, nullptr, at<int>(view, kObjectId), kSeatKind, &seat);
    if (!seat) return false;
    using Movement_t = int(__fastcall*)(void*, void*, float*, float*);
    reinterpret_cast<Movement_t>(vtable_slot(seat, kSeatMovement))(seat, nullptr, lo, hi);
    return true;
}

// Largest head movement the tracker may add, 0 without a tracker.
float tracker_bound(void* view) {
    if (!at<int>(view, kTracker)) return 0.0f;
    float lo[3] = {}, hi[3] = {};
    if (!seat_movement(view, lo, hi)) return pt::kCapSide;  // unknown: leave no room for the lean rather than guess
    float b = 0.0f;
    for (float v : {lo[0], lo[2], hi[0], hi[2]}) b = std::fabs(v) > b ? std::fabs(v) : b;
    return b;
}

void diag_log(void* view) {
    const ULONGLONG now = GetTickCount64();
    BYTE* def = at<BYTE*>(view, kViewDef);
    if (now < g_diag_next || !def || at<short>(def, kDefInsideView) == 0) return;
    g_diag_next = now + 1000;
    int axis[5] = {};
    float native_x = 0.0f, native_z = 0.0f;  // the game's own tracker head offset, camera axes, metres (as +0x2ec0)
    HMODULE vm = g_view_dll.load();
    void* im = vm ? *static_cast<void**>(at_rva(vm, kInputManagerRva)) : nullptr;
    if (im && at<int>(view, kTracker)) {
        using ReadAxis_t = int(__fastcall*)(void*, void*, int);
        for (int i = 0; i < 5; ++i) axis[i] = reinterpret_cast<ReadAxis_t>(vtable_slot(im, kReadAxis))(im, nullptr, i);
        float lo[3] = {}, hi[3] = {};
        if (seat_movement(view, lo, hi)) {
            const float kx = -axis[0] / 16383.0f, kz = axis[1] / 16383.0f;
            native_x = kx * (kx < 0 ? lo[0] : hi[0]);
            native_z = kz * (kz < 0 ? lo[2] : hi[2]);
        }
    }
    constexpr float r2d = 180.0f / pt::kPi;
    LOG("pilot twist probe: view %p applies=%d (snap=%d yawctl=%d limit=%d) yaw +0xdc=%.1f +0xac=%.1f pitch +0xd8=%.1f "
        "lim %.1f..%.1f tracker=%d mode=%d axes x=%d z=%d y=%d yaw=%d pitch=%d | camera=%d head=%.1f cont=%.1f "
        "weight=%.3f twist=%.1f eye=%d bound=%.3f lean=(%.3f,%.3f,%.3f) native tracker offset x=%.3f z=%.3f",
        view, applies_to(view) ? 1 : 0, (at<uint32_t>(def, kDefFlags) & kFlagSnapView) ? 1 : 0, at<int>(def, kDefYawControl),
        (at<uint32_t>(def, kDefFlags) & kFlagLimitYaw) ? 1 : 0, at<float>(view, kYaw) * r2d, at<float>(view, kTargetYaw) * r2d,
        at<float>(view, kYaw - 4) * r2d, at<float>(view, kMinYaw) * r2d, at<float>(view, kMaxYaw) * r2d,
        at<int>(view, kTracker), at<short>(view, kTrackerMode), axis[0], axis[1], axis[2], axis[3], axis[4],
        g_diag.camera ? 1 : 0, g_diag.head * r2d, g_diag.yaw * r2d, g_diag.weight, g_diag.delta * r2d, g_diag.eye ? 1 : 0,
        g_diag.bound, g_diag.lean[0], g_diag.lean[1], g_diag.lean[2], native_x, native_z);
}

void __fastcall h_Eye(void* view, void* edx, const float* aircraft, void* iface) {
    o_Eye(view, edx, aircraft, iface);
    if (!view || view != g_updating || view != g_last_view || !aircraft || !g_state.have || g_state.weight == 0.0f) return;
    const float bound = tracker_bound(view);
    const pt::Vec3 d = pt::body_to_world(aircraft, pt::lean_body(g_state.weight, bound));
    g_diag.eye = true;
    g_diag.bound = bound;
    g_diag.lean[0] = d.x;
    g_diag.lean[1] = d.y;
    g_diag.lean[2] = d.z;
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
    if (!still_loaded(vm)) {  // freed since: never patch whatever is mapped there now
        g_view_dll = nullptr;
        g_enabled = false;
        LOG("pilot twist: ViewManager.dll image gone; its hooks are dropped");
        return;
    }
    for (DWORD rva : kRvas) {
        MH_STATUS q = on ? MH_QueueEnableHook(at_rva(vm, rva)) : MH_QueueDisableHook(at_rva(vm, rva));
        (void)q;  // reported by MH_ApplyQueued
    }
    const MH_STATUS s = MH_ApplyQueued();
    if (s == MH_OK) g_enabled = on;
    if (s == MH_OK && !on) g_reset = true;
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

// Game thread, never under the loader lock: at game window creation (the settings are loaded by then) and whenever an
// Options page is left (in the menus, so no view update is running). Idempotent.
void pilot_twist_apply_setting() {
    AcquireSRWLockExclusive(&g_install_lock);
    set_enabled_locked(config::pilot_twist());
    ReleaseSRWLockExclusive(&g_install_lock);
}

}  // namespace LazyWorks::SF2Patch::hooks
