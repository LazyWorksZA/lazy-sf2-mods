// Object probe (developer tool, multiplayer go/no-go experiment). Inert unless the environment
// variable LAZYSF2_PROBE_DIR names a folder.
//
// It tracks the live aircraft objects (AircraftObject.dll: constructor and destructor) and, on
// Ctrl+Shift+D, dumps each object's memory and one level of the heap blocks it points to into that
// folder, so position and velocity fields can be found offline. Nothing is written to the game
// folder, and nothing is hooked or logged when the variable is not set.
//
// Hooks are at fixed RVAs and only installed on the July 2013 build.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {
namespace {

constexpr DWORD kAircraftTimestamp = 0x51d220bd, kAircraftImageSize = 0xc2000;
constexpr DWORD kCtorRva = 0x48cf0;  // AircraftObjectClass* __thiscall ctor(this, const wchar_t* name)  (ret 4)
constexpr DWORD kDtorRva = 0x41600;  // void __thiscall dtor(this)
constexpr size_t kObjectSize = 0x9a8;  // the factory allocates this much per aircraft
constexpr size_t kPointerDump = 0x400;
constexpr size_t kMaxPointerDumps = 96;
constexpr size_t kMaxTracked = 256;
constexpr size_t kNameChars = 63;

struct Entry {
    void* obj;
    wchar_t name[kNameChars + 1];
    ULONGLONG created;
};

std::wstring g_dir;  // empty = probe off; written once from DllMain, read-only afterwards
SRWLOCK g_lock = SRWLOCK_INIT;
Entry g_table[kMaxTracked];  // guarded by g_lock
size_t g_count = 0;          // guarded by g_lock
bool g_full_logged = false;  // guarded by g_lock
std::atomic<bool> g_hook_tried{false};  // build checked and hooks attempted (once)

using Ctor_t = void*(__fastcall*)(void* self, void* edx, const wchar_t* name);
using Dtor_t = void(__fastcall*)(void* self, void* edx);
Ctor_t o_Ctor = nullptr;
Dtor_t o_Dtor = nullptr;

void* __fastcall h_Ctor(void* self, void* edx, const wchar_t* name) {
    void* r = o_Ctor(self, edx, name);
    bool full = false;
    AcquireSRWLockExclusive(&g_lock);
    size_t slot = g_count;
    for (size_t i = 0; i < g_count; ++i) {
        if (g_table[i].obj == self) {  // address reused without a destructor call we saw
            slot = i;
            break;
        }
    }
    if (slot < kMaxTracked) {
        Entry& e = g_table[slot];
        e.obj = self;
        e.name[0] = L'\0';
        if (name) {
            // The game's string may be anything; copy at most kNameChars characters.
            for (size_t i = 0; i < kNameChars && name[i]; ++i) {
                e.name[i] = name[i];
                e.name[i + 1] = L'\0';
            }
        }
        e.created = GetTickCount64();
        if (slot == g_count) ++g_count;
    } else if (!g_full_logged) {
        g_full_logged = full = true;
    }
    const size_t live = g_count;
    ReleaseSRWLockExclusive(&g_lock);
    if (full) LOG("probe: aircraft table full (%zu); further aircraft are not tracked", kMaxTracked);
    LOG("probe: aircraft + %p \"%ls\" (%zu live)", self, name ? name : L"", live);
    return r;
}

void __fastcall h_Dtor(void* self, void* edx) {
    // Remove first, so a snapshot (which holds the lock while it copies) never sees an object whose
    // destructor has started.
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        if (g_table[i].obj == self) {
            g_table[i] = g_table[--g_count];
            break;
        }
    }
    const size_t live = g_count;
    ReleaseSRWLockExclusive(&g_lock);
    LOG("probe: aircraft - %p (%zu live)", self, live);
    o_Dtor(self, edx);
}

// --- Snapshot ----------------------------------------------------------------------------------

bool write_file(const std::wstring& path, const void* data, size_t n) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = n == 0 || WriteFile(f, data, static_cast<DWORD>(n), &written, nullptr);
    ok = ok && written == n;
    CloseHandle(f);
    return ok;
}

std::string to_utf8(const wchar_t* s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n) - 1);
    return out;
}

std::wstring fmt(const wchar_t* format, ...) {
    wchar_t buf[64];
    va_list ap;
    va_start(ap, format);
    vswprintf_s(buf, format, ap);
    va_end(ap);
    return buf;
}

bool read_mem(const void* from, void* to, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, n, &got) && got == n;
}

// How many bytes at p may be dumped as a "heap block": committed, private, read/write memory,
// clipped to the end of the region and to kPointerDump. 0 if p is not such memory.
size_t dumpable_bytes(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) return 0;
    const auto end = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t room = static_cast<size_t>(end - static_cast<const BYTE*>(p));
    return room < kPointerDump ? room : kPointerDump;
}

unsigned g_snapshot = 0;  // hotkey thread only

void take_snapshot() {
    const unsigned number = ++g_snapshot;
    std::string index = std::to_string(number) + " " + std::to_string(GetTickCount64()) + "\n";
    unsigned aircraft = 0, dumps = 0;

    // The lock is held (shared) across every copy: the destructor hook needs it exclusively to
    // remove an object, so no tracked object can be freed while we read it.
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        const Entry& e = g_table[i];
        std::vector<BYTE> bytes(kObjectSize);
        if (!read_mem(e.obj, bytes.data(), kObjectSize)) continue;
        const auto obj = reinterpret_cast<uintptr_t>(e.obj);
        const unsigned id = static_cast<unsigned>(obj);
        write_file(g_dir + fmt(L"\\snap%03u_%08x.bin", number, id), bytes.data(), bytes.size());
        ++aircraft;

        std::string offsets;
        size_t per_object = 0;
        for (size_t off = 0; off + 4 <= kObjectSize && per_object < kMaxPointerDumps; off += 4) {
            uint32_t value;
            memcpy(&value, bytes.data() + off, 4);
            if (value < 0x10000) continue;
            if (value >= obj && value < obj + kObjectSize) continue;  // the object itself or inside it
            const void* target = reinterpret_cast<const void*>(static_cast<uintptr_t>(value));
            const size_t n = dumpable_bytes(target);
            if (!n) continue;
            std::vector<BYTE> block(n);
            if (!read_mem(target, block.data(), n)) continue;
            write_file(g_dir + fmt(L"\\snap%03u_%08x_f%04zx.bin", number, id, off), block.data(), n);
            char tag[16];
            sprintf_s(tag, "%s%zx", offsets.empty() ? "" : ",", off);
            offsets += tag;
            ++per_object;
            ++dumps;
        }
        uint32_t vtable;
        memcpy(&vtable, bytes.data(), 4);
        char head[16], tail[48];
        sprintf_s(head, "%08x ", id);
        sprintf_s(tail, " %llu %08x ", e.created, vtable);
        index += head;
        index += to_utf8(e.name);
        index += tail;
        index += offsets;
        index += "\n";
    }
    ReleaseSRWLockShared(&g_lock);

    write_file(g_dir + fmt(L"\\snap%03u.txt", number), index.data(), index.size());
    LOG("probe: snapshot %u: %u aircraft, %u pointer dumps -> %ls", number, aircraft, dumps, g_dir.c_str());
}

DWORD WINAPI hotkey_thread(LPVOID) {
    bool was_down = false;
    for (;;) {
        Sleep(50);
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                          (GetAsyncKeyState('D') & 0x8000);
        if (down && !was_down) take_snapshot();
        was_down = down;
    }
}

}  // namespace

void install_probe_hooks() {
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LAZYSF2_PROBE_DIR", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        LOG("probe: off (LAZYSF2_PROBE_DIR not set)");
        return;
    }
    g_dir = dir;
    while (g_dir.size() > 1 && (g_dir.back() == L'\\' || g_dir.back() == L'/')) g_dir.pop_back();
    CreateDirectoryW(g_dir.c_str(), nullptr);  // fine if it exists
    LOG("probe: on, dumping to %ls (Ctrl+Shift+D takes a snapshot)", g_dir.c_str());
}

void probe_on_modules_loaded() {
    if (g_dir.empty() || g_hook_tried.load()) return;
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (!m || g_hook_tried.exchange(true)) return;
    if (!is_expected_build(m, kAircraftTimestamp, kAircraftImageSize, "AircraftObject.dll")) return;
    bool ok = create_and_enable(at_rva(m, kCtorRva), reinterpret_cast<void*>(&h_Ctor),
                                reinterpret_cast<void**>(&o_Ctor), "AircraftObject ctor");
    ok = ok && create_and_enable(at_rva(m, kDtorRva), reinterpret_cast<void*>(&h_Dtor),
                                 reinterpret_cast<void**>(&o_Dtor), "AircraftObject dtor");
    if (!ok) return;
    if (HANDLE t = CreateThread(nullptr, 0, hotkey_thread, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        LOG("probe: could not start the hotkey thread (error %lu)", GetLastError());
    }
}

}  // namespace lazysf2::hooks
