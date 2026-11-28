// Pilot head sync: on remote avatars only, the pilot's head target angles are set from the look direction the
// player sent (JetAnim), so the head turns where that player looks. Fixed RVAs in AircraftObject.dll, July 2013
// build only (the caller checked the build). See lazy-sf2-recon: recon-notes §29 (pilot head).
#include <windows.h>

#include <MinHook.h>

#include <cstdint>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kHeadTurnRva = 0x4bba0;  // pilot system: steps the head toward its target within the limits
constexpr DWORD kAoImageSize = 0xc2000;
constexpr size_t kCtxOffset = 0x1c;  // pilot system -> context; its first field is the aircraft object
constexpr size_t kTargetYaw = 0x54, kTargetPitch = 0x58;  // floats, radians

// Only +0x4bba0 is hooked: +0x4b0d0 is a different class of the same family whose object may end before +0x58, and
// writing there corrupted the heap (two-PC run, 2026-11-28).
using Fn_t = void(__fastcall*)(void* self, void* edx);
Fn_t o_Turn = nullptr;
uintptr_t g_image = 0;  // the hooked AircraftObject.dll image
const void* g_taken[16] = {};  // pilot systems already logged (game thread)

struct Owner {
    void* obj;
    bool in_image;
};

// The aircraft object of a pilot system, and whether the system's vtable lies in the hooked image. SEH only.
Owner owner_of(void* self) {
    Owner o{nullptr, false};
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(self);
        o.in_image = vt >= g_image && vt < g_image + kAoImageSize;
        const BYTE* ctx = *reinterpret_cast<BYTE* const*>(static_cast<BYTE*>(self) + kCtxOffset);
        o.obj = ctx ? *reinterpret_cast<void* const*>(ctx) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o.obj = nullptr;
    }
    return o;
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

// Before the game's own step: a remote avatar's pilot head target is the player's look (the game then smooths and
// clamps it). Other aircraft, and avatars with no fresh look, are left alone; a system whose class or owner does
// not check out is skipped with one log line.
void apply(void* self) {
    if (!self) return;
    const Owner owner = owner_of(self);
    float yaw = 0, pitch = 0;
    if (!owner.obj || !world_head_target(owner.obj, &yaw, &pitch)) return;
    static const void* skipped = nullptr;
    if (!owner.in_image) {
        if (skipped != self) {
            skipped = self;
            LOG("pilot head: pilot system %p of avatar %p is not an AircraftObject class; skipped", self, owner.obj);
        }
        return;
    }
    if (!write_target(self, yaw, pitch)) return;
    for (const void*& t : g_taken) {
        if (t == self) return;
        if (!t) {
            t = self;
            LOG("pilot head: pilot system %p of avatar %p taken (yaw %.1f, pitch %.1f degrees)", self, owner.obj,
                yaw * 57.29578f, pitch * 57.29578f);
            return;
        }
    }
}

void __fastcall h_Turn(void* self, void* edx) {
    apply(self);
    o_Turn(self, edx);
}

}  // namespace

bool pilot_head_install(HMODULE aircraft_dll) {
    g_image = reinterpret_cast<uintptr_t>(aircraft_dll);
    for (const void*& t : g_taken) t = nullptr;
    const bool ok = create_and_enable(at_rva(aircraft_dll, kHeadTurnRva), reinterpret_cast<void*>(&h_Turn),
                                      reinterpret_cast<void**>(&o_Turn), "pilot head turn");
    if (ok) LOG("pilot head: remote pilots follow the look their player sends");
    return ok;
}

void pilot_head_remove(HMODULE aircraft_dll) {
    MH_STATUS s = MH_RemoveHook(at_rva(aircraft_dll, kHeadTurnRva));
    (void)s;  // MH_ERROR_NOT_CREATED is expected
}

}  // namespace LazyWorks::SF2Patch::hooks
