// Pilot head sync: on remote avatars only, the pilot's head target angles are set from the look direction the
// player sent (JetAnim), so the head turns where that player looks. Fixed RVAs in AircraftObject.dll, July 2013
// build only (the caller checked the build). See lazy-sf2-recon: recon-notes §29 (pilot head).
#include <windows.h>

#include <MinHook.h>

#include <atomic>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kHeadUpdateRva = 0x4b0d0;  // pilot system: picks the look mode, drifts the head target
constexpr DWORD kHeadTurnRva = 0x4bba0;    // pilot system: steps the head toward its target within the limits
constexpr size_t kCtxOffset = 0x1c;        // pilot system -> context; its first field is the aircraft object
constexpr size_t kTargetYaw = 0x54, kTargetPitch = 0x58;  // floats, radians

using Fn_t = void(__fastcall*)(void* self, void* edx);
Fn_t o_Update = nullptr;
Fn_t o_Turn = nullptr;
std::atomic<bool> g_logged{false};

// The aircraft object of a pilot system, or null. SEH only, no C++ objects.
void* owner_of(void* self) {
    __try {
        const BYTE* ctx = *reinterpret_cast<BYTE* const*>(static_cast<BYTE*>(self) + kCtxOffset);
        return ctx ? *reinterpret_cast<void* const*>(ctx) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool write_target(void* self, float yaw, float pitch) {
    __try {
        *reinterpret_cast<float*>(static_cast<BYTE*>(self) + kTargetYaw) = yaw;
        *reinterpret_cast<float*>(static_cast<BYTE*>(self) + kTargetPitch) = pitch;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Before the game's own step: the remote pilot's head target is the player's look (the game then smooths and clamps
// it as it does any head movement). Other aircraft, and avatars with no fresh look, are left alone.
void apply(void* self) {
    if (!self) return;
    void* const owner = owner_of(self);
    float yaw = 0, pitch = 0;
    if (!owner || !world_head_target(owner, &yaw, &pitch)) return;
    if (!write_target(self, yaw, pitch)) return;
    if (!g_logged.exchange(true))
        LOG("pilot head: first remote pilot %p steered (yaw %.1f, pitch %.1f degrees)", owner, yaw * 57.29578f,
            pitch * 57.29578f);
}

void __fastcall h_Update(void* self, void* edx) {
    apply(self);
    o_Update(self, edx);
    apply(self);  // the update drifts the target a little each frame
}

void __fastcall h_Turn(void* self, void* edx) {
    apply(self);
    o_Turn(self, edx);
}

}  // namespace

bool pilot_head_install(HMODULE aircraft_dll) {
    g_logged = false;
    bool ok = create_and_enable(at_rva(aircraft_dll, kHeadUpdateRva), reinterpret_cast<void*>(&h_Update),
                                reinterpret_cast<void**>(&o_Update), "pilot head update");
    ok = ok && create_and_enable(at_rva(aircraft_dll, kHeadTurnRva), reinterpret_cast<void*>(&h_Turn),
                                 reinterpret_cast<void**>(&o_Turn), "pilot head turn");
    if (ok) LOG("pilot head: remote pilots follow the look their player sends");
    return ok;
}

void pilot_head_remove(HMODULE aircraft_dll) {
    for (DWORD rva : {kHeadUpdateRva, kHeadTurnRva}) {
        MH_STATUS s = MH_RemoveHook(at_rva(aircraft_dll, rva));
        (void)s;  // MH_ERROR_NOT_CREATED is expected
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
