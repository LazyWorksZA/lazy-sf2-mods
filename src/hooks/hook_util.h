#pragma once
// Small helpers shared by the hook modules.
#include <windows.h>

#include <MinHook.h>

#include "log.h"

namespace lazysf2::hooks {

inline void* vtable_slot(void* com_object, int slot) { return (*static_cast<void***>(com_object))[slot]; }

// Creates and enables a hook; logs the outcome.
inline bool create_and_enable(void* target, void* detour, void** original, const char* what) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) s = MH_EnableHook(target);
    LOG("hook %s at %p: %s", what, target, MH_StatusToString(s));
    return s == MH_OK;
}

// Fixed-RVA hooks are only safe on the exact July 2013 build we analysed (AGENTS.md).
inline bool is_expected_build(HMODULE m, DWORD timestamp, DWORD image_size, const char* name) {
    if (!m) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(reinterpret_cast<BYTE*>(m) + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp == timestamp && nt->OptionalHeader.SizeOfImage == image_size) return true;
    LOG("%s: unexpected build (timestamp 0x%08lx, size 0x%lx); its hooks are disabled", name,
        nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    return false;
}

inline void* at_rva(HMODULE m, DWORD rva) { return reinterpret_cast<BYTE*>(m) + rva; }

}  // namespace lazysf2::hooks
