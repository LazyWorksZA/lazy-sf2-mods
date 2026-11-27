// Parked remote copies stay on the ground (handover task 2). The aircraft's ground-contact step (+0x48dd0, slot 20)
// marks every REMOTEOBJECT aircraft airborne each frame; after it, a remote copy that sits on the ground gets the bit
// cleared again, so a parked or taxiing copy is not "in the air" when its AI takes over. Game thread (the aircraft's
// update). See lazy-sf2-recon: network-recon §12.1.3.
#include <windows.h>

#include <cmath>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/jet_layout.h"
#include "hooks/world_logic.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

void* g_o_ground_contact = nullptr;

namespace {

constexpr uint32_t kRvaAoRemoteMask = 0xb7b28, kStateAirborne = 0x800000u;
constexpr size_t kObjRemoteFlags = 0x0c, kObjVel = 0x50, kObjGearHeight = 0x2fc, kObjHeight = 0x620, kObjData = 0x380,
                 kDataMinGear = 0x7c8;
uint64_t g_kept = 0;

uint32_t remote_mask() {
    static HMODULE seen = nullptr;
    static uint32_t mask = 0;
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (m != seen) {
        seen = m;
        mask = 0;
        if (m && is_expected_build(m, jet::kAoStamp, jet::kAoSize, "AircraftObject.dll (ground contact)"))
            std::memcpy(&mask, at_rva(m, kRvaAoRemoteMask), 4);
    }
    return mask;
}

// 1 cleared, 0 not a grounded remote copy, -1 faulted.
int keep_on_ground(BYTE* o, uint32_t mask) {
    __try {
        uint32_t flags = 0, state = 0;
        std::memcpy(&flags, o + kObjRemoteFlags, 4);
        if (!mask || !(flags & mask)) return 0;
        std::memcpy(&state, o + jet::kStateWord, 4);
        if (!(state & kStateAirborne)) return 0;
        float height = 0, gear = 0, min_gear = 0, v[3] = {};
        std::memcpy(&height, o + kObjHeight, 4);
        std::memcpy(&gear, o + kObjGearHeight, 4);
        const BYTE* data = *reinterpret_cast<const BYTE* const*>(o + kObjData);
        if (data) std::memcpy(&min_gear, data + kDataMinGear, 4);
        std::memcpy(v, o + kObjVel, sizeof(v));
        const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!copy_on_ground(height, gear > min_gear ? gear : min_gear, speed)) return 0;
        state &= ~kStateAirborne;
        std::memcpy(o + jet::kStateWord, &state, 4);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

}  // namespace

void __fastcall h_ground_contact(void* obj) {
    reinterpret_cast<void(__fastcall*)(void*)>(g_o_ground_contact)(obj);
    if (keep_on_ground(static_cast<BYTE*>(obj), remote_mask()) == 1 && log_milestone(++g_kept))
        LOG("world: a remote copy on the ground kept its on-ground state (%llu frames so far)",
            static_cast<unsigned long long>(g_kept));
}

}  // namespace LazyWorks::SF2Patch::hooks
