// Pilot head sync: on remote avatars only, the pilot's head is steered toward the look direction the player sent
// (JetAnim). Fixed RVAs in AircraftObject.dll, July 2013 build only (the caller checked the build).
// See lazy-sf2-recon: recon-notes §29.1 (pilot head).
#include <windows.h>

#include <MinHook.h>

#include <cstdint>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kPilotVisualRva = 0x4b340;  // AircraftPilotClass slot 6: slews the head state toward the look target
constexpr DWORD kSlewPitchRva = 0x4c400;    // thiscall(state, target, step, min, max, wrap), ret 0x14: state[0]
constexpr DWORD kSlewYawRva = 0x4c650;      // the same on state[1]
constexpr DWORD kPilotVtableRva = 0x94b0c;  // AircraftPilotClass::vftable, exact
constexpr size_t kCtxOffset = 0x1c;         // pilot system -> context; its first field is the aircraft object
constexpr size_t kStateOffset = 0x3c;       // float[2]: current head pitch, yaw (radians); the object is 0x58 bytes

using Visual_t = void(__fastcall*)(void* self, void* edx);
using Slew_t = int(__fastcall*)(void* state, void* edx, float target, float step, float lo, float hi, char wrap);
Visual_t o_Visual = nullptr;
Slew_t o_SlewPitch = nullptr;
Slew_t o_SlewYaw = nullptr;
uintptr_t g_image = 0;  // the hooked AircraftObject.dll image

// The override while one remote pilot's visual step runs (game thread only).
struct Override {
    const float* state = nullptr;
    float pitch = 0, yaw = 0;
};
Override g_ov;
const void* g_taken[16] = {};  // pilot systems already logged
int g_steer_logs = 0;          // per-second state lines written, capped
ULONGLONG g_steer_next = 0;
constexpr int kMaxSteerLogs = 60;

// The aircraft object of a pilot system whose vtable is exactly AircraftPilotClass's, else null. SEH only.
void* owner_of(void* self) {
    __try {
        if (*reinterpret_cast<const uintptr_t*>(self) != g_image + kPilotVtableRva) return nullptr;
        const BYTE* ctx = *reinterpret_cast<BYTE* const*>(static_cast<BYTE*>(self) + kCtxOffset);
        return ctx ? *reinterpret_cast<void* const*>(ctx) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void log_steer(void* self, void* owner, float yaw, float pitch) {
    const float* st = reinterpret_cast<const float*>(static_cast<BYTE*>(self) + kStateOffset);
    for (const void*& t : g_taken) {
        if (t == self) break;
        if (!t) {
            t = self;
            LOG("pilot head: pilot system %p of avatar %p taken", self, owner);
            break;
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (g_steer_logs >= kMaxSteerLogs || now < g_steer_next) return;
    g_steer_next = now + 1000;
    ++g_steer_logs;
    LOG("pilot head: %p target yaw %.1f pitch %.1f | head yaw %.1f pitch %.1f (degrees, before this step)", self,
        yaw * 57.29578f, pitch * 57.29578f, st[1] * 57.29578f, st[0] * 57.29578f);
}

void __fastcall h_Visual(void* self, void* edx) {
    void* const owner = self ? owner_of(self) : nullptr;
    float yaw = 0, pitch = 0;
    if (owner && world_head_target(owner, &yaw, &pitch)) {
        g_ov = {reinterpret_cast<const float*>(static_cast<BYTE*>(self) + kStateOffset), pitch, yaw};
        log_steer(self, owner, yaw, pitch);
        o_Visual(self, edx);
        g_ov = {};
        return;
    }
    o_Visual(self, edx);
}

// The game's slew clamps the target to its own head limits and moves the head by its pan step, so only the target
// is replaced, and only for the system being stepped now.
int __fastcall h_SlewPitch(void* state, void* edx, float target, float step, float lo, float hi, char wrap) {
    if (g_ov.state && state == g_ov.state) target = g_ov.pitch;
    return o_SlewPitch(state, edx, target, step, lo, hi, wrap);
}

int __fastcall h_SlewYaw(void* state, void* edx, float target, float step, float lo, float hi, char wrap) {
    if (g_ov.state && static_cast<const float*>(state) == g_ov.state + 1) target = g_ov.yaw;
    return o_SlewYaw(state, edx, target, step, lo, hi, wrap);
}

}  // namespace

bool pilot_head_install(HMODULE aircraft_dll) {
    g_image = reinterpret_cast<uintptr_t>(aircraft_dll);
    for (const void*& t : g_taken) t = nullptr;
    g_ov = {};
    g_steer_logs = 0;
    bool ok = create_and_enable(at_rva(aircraft_dll, kSlewPitchRva), reinterpret_cast<void*>(&h_SlewPitch),
                                reinterpret_cast<void**>(&o_SlewPitch), "pilot head slew pitch");
    ok = ok && create_and_enable(at_rva(aircraft_dll, kSlewYawRva), reinterpret_cast<void*>(&h_SlewYaw),
                                 reinterpret_cast<void**>(&o_SlewYaw), "pilot head slew yaw");
    ok = ok && create_and_enable(at_rva(aircraft_dll, kPilotVisualRva), reinterpret_cast<void*>(&h_Visual),
                                 reinterpret_cast<void**>(&o_Visual), "pilot head visual");
    if (ok) LOG("pilot head: remote pilots follow the look their player sends");
    return ok;
}

void pilot_head_remove(HMODULE aircraft_dll) {
    for (DWORD rva : {kPilotVisualRva, kSlewPitchRva, kSlewYawRva}) {
        MH_STATUS s = MH_RemoveHook(at_rva(aircraft_dll, rva));
        (void)s;  // MH_ERROR_NOT_CREATED is expected
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
