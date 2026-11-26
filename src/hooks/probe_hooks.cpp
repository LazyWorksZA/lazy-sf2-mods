// Object probe (developer tool): inert unless LAZYSF2_PROBE_DIR names a folder. Ctrl+Shift+D dumps live aircraft
// object memory there; also starts the other developer features. Nothing is written to the game folder.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/probe_snap_logic.h"
#include "hooks/terrain_export.h"
#include "hooks/world_walk.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr size_t kObjectSize = 0x9a8;  // the factory allocates this much per aircraft
constexpr size_t kPointerDump = 0x400;
constexpr size_t kMaxPointerDumps = 96;
constexpr size_t kNameChars = 63;

std::wstring g_dir;  // empty = probe off; written once from DllMain, read-only afterwards
std::atomic<bool> g_dev_started{false};  // developer features (recorder, replay, host, hotkeys) started (once)

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

std::atomic<unsigned> g_snapshot{0};  // last snapshot number; set once from the folder at start, then manual and auto count up

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
struct CopyCtx {
    std::vector<Copy> copies;
    size_t limit = SIZE_MAX;  // most aircraft to copy: the player first, then the others in table order
    size_t omitted = 0;
    bool failed = false;      // an allocation failed: copies was cleared
};

void copy_one(const LiveAircraft& e, std::vector<Copy>& copies) {
    Copy c;
    c.object.resize(kObjectSize);
    if (!read_mem(e.obj, c.object.data(), kObjectSize)) return;
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

// No exception may leave this function: the caller releases the table lock only after it returns normally.
void copy_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& ctx = *static_cast<CopyCtx*>(vctx);
    try {
        size_t taken = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < n; ++i) {
                if (list[i].is_player != (pass == 0)) continue;
                if (taken >= ctx.limit) {
                    ++ctx.omitted;
                    continue;
                }
                ++taken;
                copy_one(list[i], ctx.copies);
            }
        }
    } catch (...) {
        ctx.copies.clear();
        ctx.copies.shrink_to_fit();
        ctx.failed = true;
    }
}

/// <summary>Writes snapshot number: the index, the aircraft dumps and, for an automatic one, world (the object table and terrain text).</summary>
unsigned write_snapshot(unsigned number, const wchar_t* tag, const std::vector<Copy>& copies, const std::string* world) {
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
        if (put(g_dir + fmt(L"\\snap%03u%ls_%08x.bin", number, tag, c.id), c.object.data(), c.object.size())) ++aircraft;
        std::string offsets;
        for (const Block& b : c.blocks) {
            if (!put(g_dir + fmt(L"\\snap%03u%ls_%08x_f%04zx.bin", number, tag, c.id, b.offset), b.bytes.data(),
                     b.bytes.size())) {
                continue;
            }
            char tag_text[16];
            sprintf_s(tag_text, "%s%zx", offsets.empty() ? "" : ",", b.offset);
            offsets += tag_text;
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
    if (world) put(g_dir + fmt(L"\\snap%03u%ls_world.txt", number, tag), world->data(), world->size());

    put(g_dir + fmt(L"\\snap%03u%ls.txt", number, tag), index.data(), index.size());
    const char* kind = *tag ? "auto " : "";
    if (errors) {
        LOG("probe: %ssnapshot %u: %u aircraft, %u pointer dumps -> %ls, %u write errors (last error %lu)", kind, number,
            aircraft, dumps, g_dir.c_str(), errors, last_error);
    } else {
        LOG("probe: %ssnapshot %u: %u aircraft, %u pointer dumps -> %ls", kind, number, aircraft, dumps, g_dir.c_str());
    }
    return errors;
}

void take_snapshot() {
    const unsigned number = ++g_snapshot;
    CopyCtx ctx;
    probe_with_aircraft(copy_cb, &ctx);
    if (ctx.failed) {
        LOG("probe: snapshot %u: out of memory; dropped", number);
        return;
    }
    write_snapshot(number, L"", ctx.copies, nullptr);
}

// Automatic snapshot, ten seconds into each flight.
constexpr double kAutoDelaySeconds = 10.0;
constexpr size_t kAutoKeepFlights = 5;
constexpr size_t kAutoMaxAircraft = 32;     // the player and the first others: bounds the work done in one frame
constexpr unsigned kMaxSnapshotNumber = 99999;  // a larger number in the folder is not ours: ignored when seeding

// Highest snapshot number already in the folder, so a new session never overwrites an old snapshot.
unsigned scan_highest_snapshot() {
    unsigned highest = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\snap*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        bool is_auto;
        const long n = probe_snapshot_number(fd.cFileName, &is_auto);
        if (n > 0 && static_cast<unsigned>(n) <= kMaxSnapshotNumber && static_cast<unsigned>(n) > highest) highest = static_cast<unsigned>(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return highest;
}

// Deletes the files of all but the newest kAutoKeepFlights automatic snapshots. Only names matching snapNNN_auto.
void prune_auto_snapshots() {
    std::vector<long> numbers;
    std::vector<std::pair<long, std::wstring>> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\snap*_auto*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        bool is_auto;
        const long n = probe_snapshot_number(fd.cFileName, &is_auto);
        if (!is_auto || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        numbers.push_back(n);
        files.emplace_back(n, g_dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    const std::vector<long> old = probe_auto_numbers_to_delete(numbers, kAutoKeepFlights);
    for (const auto& f : files)
        if (std::find(old.begin(), old.end(), f.first) != old.end()) DeleteFileW(f.second.c_str());
}

std::atomic<HANDLE> g_auto_writer{nullptr};  // the latest writer thread; closed once it has ended

// True if no writer is running. Never waits: the game thread must not stall on a slow disk.
bool writer_idle() {
    HANDLE h = g_auto_writer.load();
    if (!h) return true;
    if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) return false;
    CloseHandle(g_auto_writer.exchange(nullptr));
    return true;
}

// DLL detach: waits at most 1 s for a running writer (none if it has already ended), then leaves it.
void retire_writer(HANDLE h) {
    if (!h) return;
    WaitForSingleObject(h, 1000);
    CloseHandle(h);
}

struct AutoJob {
    unsigned number = 0;
    std::vector<Copy> copies;
    std::string world;
};

DWORD WINAPI auto_snapshot_thread(LPVOID p) {
    std::unique_ptr<AutoJob> job(static_cast<AutoJob*>(p));
    if (write_snapshot(job->number, L"_auto", job->copies, &job->world) == 0) prune_auto_snapshots();
    return 0;
}

// Game thread: the memory is read here (the object table and terrain readers are game-thread only); the files are
// written by a short-lived thread. Developer-only: the copy of up to kAutoMaxAircraft aircraft (about 100 KB and 200
// system calls each) and the world text (capped lines) run in one frame, so the frame may hitch. An exception drops
// the snapshot. See lazy-sf2-recon: recon-notes §20.
void take_auto_snapshot(double game_time) {
    static std::atomic<bool> logged_busy{false}, logged_fail{false};
    if (!writer_idle()) {  // a slow disk: skip this flight rather than stall or pile up writers
        if (!logged_busy.exchange(true)) LOG("probe: the previous auto snapshot is still being written; skipping this one");
        return;
    }
    try {
        std::unique_ptr<AutoJob> job(new AutoJob);
        job->number = ++g_snapshot;
        CopyCtx ctx;
        ctx.limit = kAutoMaxAircraft;
        probe_with_aircraft(copy_cb, &ctx);
        if (ctx.failed) throw std::bad_alloc();
        job->copies = std::move(ctx.copies);
        char line[160];
        sprintf_s(line, "auto snapshot %u at game time %.1f s\n", job->number, game_time);
        job->world = line;
        if (ctx.omitted) {
            sprintf_s(line, "aircraft: %zu copied, %zu omitted (cap %zu, the player first)\n", job->copies.size(),
                      ctx.omitted, kAutoMaxAircraft);
            job->world += line;
        }
        world_walk_summary(job->world);
        TerrainInfo terrain;
        if (terrain_basics(terrain)) {
            sprintf_s(line, "terrain: id=%s size=%dx%d spacing=%.1f geo=%s", terrain.id.c_str(), terrain.w, terrain.h,
                      terrain.spacing, terrain.has_geo ? "yes" : "no");
            job->world += line;
            if (terrain.has_geo) {
                sprintf_s(line, " lon=%.5f lat=%.5f", terrain.geo_lon, terrain.geo_lat);
                job->world += line;
            }
            job->world += " name=" + terrain.full_name + "\n";
        } else {
            job->world += "terrain: not available\n";
        }
        job->world += "mission (MSN): not read (no safe accessor yet)\n";
        const unsigned number = job->number;
        if (HANDLE t = CreateThread(nullptr, 0, auto_snapshot_thread, job.get(), 0, nullptr)) {
            job.release();
            g_auto_writer.store(t);
        } else {
            LOG("probe: auto snapshot %u: could not start the writer thread (error %lu)", number, GetLastError());
        }
    } catch (...) {
        if (!logged_fail.exchange(true)) LOG("probe: an auto snapshot ran out of memory and was dropped");
    }
}

uint32_t g_auto_epoch = 0xffffffffu;  // game thread only
bool g_auto_pending = false;
double g_auto_start_game = 0;
ULONGLONG g_auto_start_wall = 0;

// Step callback: a new player epoch is a new flight; the snapshot is due kAutoDelaySeconds later (wall time if the
// game clock is not usable).
void auto_step(void*, float, bool is_player) {
    if (!is_player || !sim_player_known()) return;
    const uint32_t epoch = sim_player_epoch();
    const double t = sim_game_time();
    if (epoch != g_auto_epoch) {
        g_auto_epoch = epoch;
        g_auto_pending = true;
        g_auto_start_game = t;
        g_auto_start_wall = GetTickCount64();
    }
    if (!g_auto_pending) return;
    const bool clock_ok = std::isfinite(t) && t >= g_auto_start_game;
    const double elapsed = clock_ok ? t - g_auto_start_game
                                    : static_cast<double>(GetTickCount64() - g_auto_start_wall) / 1000.0;
    if (elapsed < kAutoDelaySeconds) return;
    g_auto_pending = false;
    take_auto_snapshot(t);
}

bool game_in_front() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

DWORD WINAPI hotkey_thread(LPVOID) {
    bool was_down = false, puppet_was_down = false, missile_was_down = false, rec_was_down = false,
         replay_was_down = false, rate_was_down = false, jump_was_down = false, nudge_was_down = false,
         twist_was_down = false, hold_was_down = false, destroy_was_down = false, place_was_down = false,
         lock_was_down = false, ai_lock_was_down = false;
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
        const bool jump_down = mods && (GetAsyncKeyState('J') & 0x8000);
        if (jump_down && !jump_was_down) player_jump_request(true);
        jump_was_down = jump_down;
        const bool nudge_down = mods && (GetAsyncKeyState('K') & 0x8000);
        if (nudge_down && !nudge_was_down) player_jump_request(false);
        nudge_was_down = nudge_down;
        const bool twist_down = mods && (GetAsyncKeyState('W') & 0x8000);
        if (twist_down && !twist_was_down) pilot_twist_dev_toggle();
        twist_was_down = twist_down;
        const bool hold_down = mods && (GetAsyncKeyState('H') & 0x8000);
        if (hold_down && !hold_was_down) pilot_twist_dev_cycle_hold();
        hold_was_down = hold_down;
        const bool destroy_down = mods && (GetAsyncKeyState('X') & 0x8000);
        if (destroy_down && !destroy_was_down) tacview_destroy_nearest_request();
        destroy_was_down = destroy_down;
        const bool place_down = mods && (GetAsyncKeyState('A') & 0x8000);
        if (place_down && !place_was_down) lock_probe_request(1);
        place_was_down = place_down;
        const bool lock_down = mods && (GetAsyncKeyState('O') & 0x8000);
        if (lock_down && !lock_was_down) lock_probe_request(2);
        lock_was_down = lock_down;
        const bool ai_lock_down = mods && (GetAsyncKeyState('I') & 0x8000);
        if (ai_lock_down && !ai_lock_was_down) lock_probe_request(3);
        ai_lock_was_down = ai_lock_down;
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

void probe_shutdown() { retire_writer(g_auto_writer.exchange(nullptr)); }

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
    player_jump_on_modules_loaded();
    lock_probe_on_modules_loaded();
    step_order_on_modules_loaded();
    g_snapshot = scan_highest_snapshot();
    if (sim_register_step_callback(auto_step)) {
        LOG("probe: an automatic snapshot is taken %.0f s into each flight (the newest %zu flights are kept)",
            kAutoDelaySeconds, kAutoKeepFlights);
    } else {
        LOG("probe: no free step callback slot; no automatic snapshots");
    }
    host_on_modules_loaded();      // starts the network thread (once), only with LAZYSF2_HOST
    if (HANDLE t = CreateThread(nullptr, 0, hotkey_thread, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        LOG("probe: could not start the hotkey thread (error %lu)", GetLastError());
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
