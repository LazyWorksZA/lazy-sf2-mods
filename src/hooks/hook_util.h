#pragma once
// Small helpers shared by the hook modules.
#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

inline void* vtable_slot(void* com_object, int slot) { return (*static_cast<void***>(com_object))[slot]; }

/// <summary>Text in the game's ANSI code page (theatre names, our ASCII wire strings) as UTF-16.</summary>
inline std::wstring widen_ansi(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}
inline std::wstring widen_ansi(const std::string& s) { return widen_ansi(s.c_str()); }

/// <summary>Creates and enables a hook; logs the outcome.</summary>
inline bool create_and_enable(void* target, void* detour, void** original, const char* what) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) s = MH_EnableHook(target);
    LOG("hook %s at %p: %s", what, target, MH_StatusToString(s));
    return s == MH_OK;
}

/// <summary>Fixed-RVA hooks are only safe on the exact July 2013 build we analysed (AGENTS.md).</summary>
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

/// <summary>"Module.dll+0x1234" for a code address (e.g. a caller's return address), for diagnostics.</summary>
inline void describe_address(const void* addr, char* out, size_t n) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
    }
    const char* base = strrchr(name, '\\');
    _snprintf_s(out, n, _TRUNCATE, "%s+0x%lx", base ? base + 1 : name,
                static_cast<unsigned long>(static_cast<const BYTE*>(addr) - reinterpret_cast<const BYTE*>(m)));
}

}  // namespace LazyWorks::SF2Patch::hooks
