// Pilot Twist (Options > Controls, default Off): in the cockpit view, past about 60 degrees of head yaw the view turns
// further (torso twist) and the eye moves a few centimetres toward that side and up. Hooks ViewManager.dll's inside
// view at fixed RVAs. See lazy-sf2-recon: recon-notes §25.
#include <windows.h>

#include <MinHook.h>

#include <atomic>
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

SRWLOCK g_install_lock = SRWLOCK_INIT;
HMODULE g_view_dll = nullptr;  // image carrying our hooks
HMODULE g_bad_base = nullptr;  // image already found unusable (checked once)
std::atomic<bool> g_logged_on{false};

// Game thread only (the view update).
void* g_updating = nullptr;   // the inside view whose update is running, else nullptr
const void* g_last_view = nullptr;
float g_weight = 0.0f;        // filtered signed strength
LARGE_INTEGER g_last_tick{};
bool g_have_state = false;

template <typename T>
T& at(void* p, int offset) {
    return *reinterpret_cast<T*>(static_cast<BYTE*>(p) + offset);
}

// The view is an inside (cockpit) view that the player turns with hat or tracker, not a fixed snap view.
bool applies_to(void* view) {
    BYTE* def = at<BYTE*>(view, kViewDef);
    return def && at<short>(def, kDefInsideView) != 0 && (at<uint32_t>(def, kDefFlags) & kFlagSnapView) == 0;
}

float seconds_since_last() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = g_have_state ? static_cast<float>(now.QuadPart - g_last_tick.QuadPart) / freq.QuadPart : pt::kSnapAfterSeconds;
    g_last_tick = now;
    return dt;
}

void __fastcall h_Update(void* view, void* edx, float dt) {
    const bool on = config::pilot_twist();
    if (on != g_logged_on.exchange(on)) LOG("pilot twist: %s", on ? "on" : "off");
    if (!on) {
        g_have_state = false;
        o_Update(view, edx, dt);
        return;
    }
    g_updating = applies_to(view) ? view : nullptr;
    o_Update(view, edx, dt);
    g_updating = nullptr;
}

// Twist without feedback: the extra yaw is written into the view's yaw only while the original builds this frame's
// camera matrix, then the head yaw is put back, so the game's smoothing and limits never see it.
void __fastcall h_Camera(void* view, void* edx, const float* aircraft) {
    if (view != g_updating) {
        o_Camera(view, edx, aircraft);
        return;
    }
    float& yaw = at<float>(view, kYaw);
    const float head = yaw;
    const float dt = seconds_since_last();
    const float target = pt::target_weight(head);
    g_weight = (g_have_state && view == g_last_view) ? pt::filter_step(g_weight, target, dt) : target;
    g_last_view = view;
    g_have_state = true;

    BYTE* def = at<BYTE*>(view, kViewDef);
    const bool limited = (at<uint32_t>(def, kDefFlags) & kFlagLimitYaw) != 0;
    yaw = pt::twisted_yaw(head, g_weight, at<float>(view, kMinYaw), at<float>(view, kMaxYaw), limited);
    o_Camera(view, edx, aircraft);
    yaw = head;
}

// Largest head movement the tracker may add (seat MinMovement/MaxMovement), 0 without a tracker.
float tracker_bound(void* view) {
    if (!at<int>(view, kTracker) || !g_view_dll) return 0.0f;
    void* om = *static_cast<void**>(at_rva(g_view_dll, kObjectManagerRva));
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
    if (view != g_updating || view != g_last_view || !aircraft || g_weight == 0.0f) return;
    const pt::Vec3 d = pt::body_to_world(aircraft, pt::lean_body(g_weight, tracker_bound(view)));
    at<double>(view, kEye) += d.x;
    at<double>(view, kEye + 8) += d.y;
    at<double>(view, kEye + 16) += d.z;
}

void remove_view_hooks(HMODULE base) {
    for (DWORD rva : {kUpdateRva, kCameraRva, kEyeRva}) {
        MH_STATUS s = MH_RemoveHook(at_rva(base, rva));
        (void)s;  // MH_ERROR_NOT_CREATED is expected
    }
}

// True if base is still a loaded image (not freed). Compared by handle only: the name was checked when hooked.
bool still_loaded(HMODULE base) {
    HMODULE h = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(base), &h) &&
           h == base && GetModuleHandleW(L"ViewManager.dll") == base;
}

}  // namespace

// Runs under the module watch (maybe under the loader lock): no settings are read here. The hooks stay in place
// and check the setting per call.
void pilot_twist_on_modules_loaded() {
    HMODULE vm = GetModuleHandleW(L"ViewManager.dll");
    if (!vm) return;
    const auto* entry = static_cast<const BYTE*>(at_rva(vm, kUpdateRva));
    if (vm == g_view_dll && *entry == 0xE9) return;  // already hooked (MinHook writes a JMP rel32)
    if (vm == g_bad_base) return;

    AcquireSRWLockExclusive(&g_install_lock);
    if (!(vm == g_view_dll && *entry == 0xE9) && vm != g_bad_base) {
        const bool build_ok = is_expected_build(vm, kViewTimestamp, kViewImageSize, "ViewManager.dll");
        if (g_view_dll) {
            // MinHook restores the original bytes on removal: only while the old image is still this verified DLL.
            if (build_ok && still_loaded(g_view_dll)) remove_view_hooks(g_view_dll);
            g_view_dll = nullptr;
        }
        if (build_ok) {
            remove_view_hooks(vm);  // entries left by an earlier image at this base
            g_view_dll = vm;
            bool ok = create_and_enable(at_rva(vm, kUpdateRva), reinterpret_cast<void*>(&h_Update),
                                        reinterpret_cast<void**>(&o_Update), "ViewManager.dll+0x3360 (inside view update)");
            ok = ok && create_and_enable(at_rva(vm, kCameraRva), reinterpret_cast<void*>(&h_Camera),
                                         reinterpret_cast<void**>(&o_Camera), "ViewManager.dll+0x3070 (view camera)");
            ok = ok && create_and_enable(at_rva(vm, kEyeRva), reinterpret_cast<void*>(&h_Eye),
                                         reinterpret_cast<void**>(&o_Eye), "ViewManager.dll+0x2ec0 (view eye)");
            if (!ok) {
                remove_view_hooks(vm);
                g_view_dll = nullptr;
                g_bad_base = vm;
                LOG("pilot twist: unavailable (hooks not installed)");
            }
        } else {
            g_bad_base = vm;
            LOG("pilot twist: unavailable (ViewManager.dll not recognised)");
        }
    }
    ReleaseSRWLockExclusive(&g_install_lock);
}

}  // namespace LazyWorks::SF2Patch::hooks
