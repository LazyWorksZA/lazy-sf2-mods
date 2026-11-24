// Object probe (developer tool, multiplayer go/no-go experiment). Inert unless the environment
// variable LAZYSF2_PROBE_DIR names a folder.
//
// It builds on the aircraft tracker (aircraft_track.cpp, which owns the AircraftObject.dll hooks, the table and
// the player rule). On Ctrl+Shift+D it dumps each object's memory and one level of the heap blocks it points to into that
// folder, so position and velocity fields can be found offline. Nothing is written to the game
// folder, and nothing is hooked or logged when the variable is not set.
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

constexpr size_t kObjectSize = 0x9a8;  // the factory allocates this much per aircraft
constexpr size_t kPointerDump = 0x400;
constexpr size_t kMaxPointerDumps = 96;
constexpr size_t kNameChars = 63;

std::wstring g_dir;  // empty = probe off; written once from DllMain, read-only afterwards
std::atomic<bool> g_dev_started{false};  // developer features (recorder, replay, host, hotkeys) started (once)

// --- Snapshot ----------------------------------------------------------------------------------

bool write_file(const std::wstring& path, const void* data, size_t n, DWORD* error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }
    DWORD written = 0;
    bool ok = n == 0 || WriteFile(f, data, static_cast<DWORD>(n), &written, nullptr);
    ok = ok && written == n;
    *error = ok ? 0 : (GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);  // before CloseHandle
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

struct Block {
    size_t offset;
    std::vector<BYTE> bytes;
};
struct Copy {
    unsigned id = 0;
    wchar_t name[kNameChars + 1] = {};
    ULONGLONG created = 0;
    std::vector<BYTE> object;
    std::vector<Block> blocks;
};

// Under the table's shared lock we only copy memory (ReadProcessMemory, VirtualQuery) into heap buffers. The
// destructor hook needs the lock exclusively to remove an object, so no listed object can be freed while we read
// it. Files are written after the lock is released.
void copy_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& copies = *static_cast<std::vector<Copy>*>(vctx);
    for (size_t i = 0; i < n; ++i) {
        const LiveAircraft& e = list[i];
        Copy c;
        c.object.resize(kObjectSize);
        if (!read_mem(e.obj, c.object.data(), kObjectSize)) continue;
        const auto obj = reinterpret_cast<uintptr_t>(e.obj);
        c.id = static_cast<unsigned>(obj);
        wcsncpy_s(c.name, e.name, _TRUNCATE);
        c.created = e.created_ms;
        for (size_t off = 0; off + 4 <= kObjectSize && c.blocks.size() < kMaxPointerDumps; off += 4) {
            uint32_t value;
            memcpy(&value, c.object.data() + off, 4);
            if (value < 0x10000) continue;
            if (value >= obj && value < obj + kObjectSize) continue;  // the object itself or inside it
            const void* target = reinterpret_cast<const void*>(static_cast<uintptr_t>(value));
            const size_t bytes = dumpable_bytes(target);
            if (!bytes) continue;
            Block b{off, std::vector<BYTE>(bytes)};
            if (!read_mem(target, b.bytes.data(), bytes)) continue;
            c.blocks.push_back(std::move(b));
        }
        copies.push_back(std::move(c));
    }
}

void take_snapshot() {
    const unsigned number = ++g_snapshot;
    std::vector<Copy> copies;
    probe_with_aircraft(copy_cb, &copies);

    std::string index = std::to_string(number) + " " + std::to_string(GetTickCount64()) + "\n";
    unsigned aircraft = 0, dumps = 0, errors = 0;
    DWORD last_error = 0;
    auto put = [&](const std::wstring& path, const void* data, size_t n) {
        DWORD err = 0;
        if (write_file(path, data, n, &err)) return true;
        ++errors;
        last_error = err;
        return false;
    };
    for (const Copy& c : copies) {
        if (put(g_dir + fmt(L"\\snap%03u_%08x.bin", number, c.id), c.object.data(), c.object.size())) ++aircraft;
        std::string offsets;
        for (const Block& b : c.blocks) {
            if (!put(g_dir + fmt(L"\\snap%03u_%08x_f%04zx.bin", number, c.id, b.offset), b.bytes.data(),
                     b.bytes.size())) {
                continue;
            }
            char tag[16];
            sprintf_s(tag, "%s%zx", offsets.empty() ? "" : ",", b.offset);
            offsets += tag;
            ++dumps;
        }
        // ptr created_tick vtable offsets name  (the name is last: it may contain spaces)
        uint32_t vtable;
        memcpy(&vtable, c.object.data(), 4);
        char line[64];  // fixed-size fields only; the offsets list is appended to the string directly
        sprintf_s(line, "%08x %llu %08x ", c.id, c.created, vtable);
        index += line;
        index += offsets.empty() ? "-" : offsets;
        index += ' ';
        index += to_utf8(c.name);
        index += "\n";
    }

    put(g_dir + fmt(L"\\snap%03u.txt", number), index.data(), index.size());
    if (errors) {
        LOG("probe: snapshot %u: %u aircraft, %u pointer dumps -> %ls, %u write errors (last error %lu)", number,
            aircraft, dumps, g_dir.c_str(), errors, last_error);
    } else {
        LOG("probe: snapshot %u: %u aircraft, %u pointer dumps -> %ls", number, aircraft, dumps, g_dir.c_str());
    }
}

bool game_in_front() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

DWORD WINAPI hotkey_thread(LPVOID) {
    bool was_down = false, puppet_was_down = false, missile_was_down = false, rec_was_down = false,
         replay_was_down = false, rate_was_down = false;
    for (;;) {
        Sleep(50);
        const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                          game_in_front();
        const bool down = mods && (GetAsyncKeyState('D') & 0x8000);
        if (down && !was_down) take_snapshot();
        was_down = down;
        const bool puppet_down = mods && (GetAsyncKeyState('P') & 0x8000);
        if (puppet_down && !puppet_was_down) puppet_toggle();
        puppet_was_down = puppet_down;
        const bool missile_down = mods && (GetAsyncKeyState('M') & 0x8000);
        if (missile_down && !missile_was_down) missile_spawn_request();
        missile_was_down = missile_down;
        const bool rec_down = mods && (GetAsyncKeyState('R') & 0x8000);
        if (rec_down && !rec_was_down) recorder_toggle();
        rec_was_down = rec_down;
        const bool replay_down = mods && (GetAsyncKeyState('T') & 0x8000);
        if (replay_down && !replay_was_down) replay_toggle();
        replay_was_down = replay_down;
        const bool rate_down = mods && (GetAsyncKeyState('Y') & 0x8000);
        if (rate_down && !rate_was_down) replay_cycle_rate();
        rate_was_down = rate_down;
    }
}

}  // namespace

namespace {

// True if path is dir itself or inside it (case-insensitive, separator boundary).
bool inside_folder(const std::wstring& path, std::wstring dir) {
    while (dir.size() > 1 && dir.back() == L'\\') dir.pop_back();
    if (path.size() < dir.size()) return false;
    if (CompareStringOrdinal(path.c_str(), static_cast<int>(dir.size()), dir.c_str(), static_cast<int>(dir.size()),
                             TRUE) != CSTR_EQUAL) {
        return false;
    }
    return path.size() == dir.size() || path[dir.size()] == L'\\' || dir.back() == L'\\';
}

bool is_absolute(const wchar_t* p) {
    const bool drive = ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' &&
                       (p[2] == L'\\' || p[2] == L'/');
    const bool unc = (p[0] == L'\\' && p[1] == L'\\') || (p[0] == L'/' && p[1] == L'/');
    return drive || unc;
}

// Absolute path of an existing directory that is not the game folder or inside it.
bool valid_probe_dir(const wchar_t* value, std::wstring& out) {
    if (!is_absolute(value)) return false;
    wchar_t full[MAX_PATH];
    const DWORD n = GetFullPathNameW(value, MAX_PATH, full, nullptr);
    if (n == 0 || n >= MAX_PATH) return false;
    const DWORD attrs = GetFileAttributesW(full);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    wchar_t exe[MAX_PATH];
    const DWORD en = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (en == 0 || en >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    if (inside_folder(full, exe)) return false;
    out = full;
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return true;
}

}  // namespace

const wchar_t* probe_output_dir() { return g_dir.empty() ? nullptr : g_dir.c_str(); }

void install_probe_hooks() {
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LAZYSF2_PROBE_DIR", dir, MAX_PATH);
    if (n == 0) {
        LOG("probe: off (LAZYSF2_PROBE_DIR not set)");
        return;
    }
    if (n >= MAX_PATH || !valid_probe_dir(dir, g_dir)) {
        g_dir.clear();
        LOG("probe: off (LAZYSF2_PROBE_DIR is not an existing directory outside the game folder)");
        return;
    }
    LOG("probe: on, dumping to %ls (Ctrl+Shift+D takes a snapshot)", g_dir.c_str());
}

void probe_on_modules_loaded() {
    if (g_dir.empty()) return;
    missile_spawn_on_modules_loaded();  // own build check and once-flag
    if (g_dev_started.load() || !aircraft_track_active()) return;  // the tracker hooks the aircraft first
    if (g_dev_started.exchange(true)) return;
    recorder_on_modules_loaded();  // registers its step callback (once)
    replay_on_modules_loaded();    // likewise
    host_on_modules_loaded();      // starts the network thread (once), only with LAZYSF2_HOST
    if (HANDLE t = CreateThread(nullptr, 0, hotkey_thread, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        LOG("probe: could not start the hotkey thread (error %lu)", GetLastError());
    }
}

}  // namespace lazysf2::hooks
