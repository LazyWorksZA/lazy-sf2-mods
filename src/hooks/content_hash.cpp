// The build check the lobby compares between host and clients (L4): the exe's build and the mod's protocol. What a
// mission uses (aircraft, weapons, theatre, ground objects) is compared per mission instead (mission_content.cpp), so
// mods a mission does not touch may differ between PCs.
#include <windows.h>

#include "hooks/hooks.h"
#include "log.h"
#include "net/protocol.h"

namespace LazyWorks::SF2Patch::hooks {

uint32_t content_hash() {
    uint32_t h = 0x811C9DC5u;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 0x01000193u;
        }
    };
    mix(net::kVersion);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const BYTE*>(dos) + dos->e_lfanew);
    mix(nt->FileHeader.TimeDateStamp);
    mix(nt->OptionalHeader.SizeOfImage);
    const uint32_t hash = h ? h : 1u;
    LOG("lobby: build check 0x%08lx (game build and mod protocol v%u)", static_cast<unsigned long>(hash),
        static_cast<unsigned>(net::kVersion));
    return hash;
}

}  // namespace LazyWorks::SF2Patch::hooks
