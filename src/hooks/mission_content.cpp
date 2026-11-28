// The files behind a mission's content list (mission_content.h): read from the player's own folder first (mods),
// then through the exe's file manager (loose files in the game folder, then the cats), as the game finds them.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "acmi/airfields.h"
#include "config.h"
#include "hooks/game_files.h"
#include "hooks/gen_filter.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/mission_content.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

std::wstring game_dir() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring d(exe);
    d.resize(d.find_last_of(L'\\'));
    return d;
}

bool read_loose(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= static_cast<LONGLONG>(kMaxGameFileBytes);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    }
    CloseHandle(h);
    return ok;
}

// Where the first file of the entry being hashed was found (for the log): the game's own order, see
// lazy-sf2-recon: recon-notes §21.4 and the start-up table (the player's folder overrides the game folder; the file
// manager's Open +0x94e0 tries the loose file, then the mounted cats by bare name, newest cat first).
thread_local const char* g_source = nullptr;  // per thread: the inventory is built on a worker

// rel: a path under the game folder, as "Objects\Aircraft\F-4E\F-4E.ini".
bool read_content_file(const std::wstring& rel, std::vector<uint8_t>* out) {
    if (rel.size() > 2 && rel[1] == L':') {  // a type record's own folder (absolute): that loose file, else the cats
        const DWORD a = GetFileAttributesW(rel.c_str());
        const bool loose = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        if (!(loose ? read_loose(rel, out) : read_game_file(rel, *out))) return false;
        if (!g_source) {
            const std::wstring user = config::game_data_dir();
            g_source = !loose ? "a cat"
                       : !user.empty() && _wcsnicmp(rel.c_str(), user.c_str(), user.size()) == 0 ? "mod folder"
                                                                                               : "game folder";
        }
        return true;
    }
    const std::wstring user = config::game_data_dir();
    if (!user.empty() && read_loose(user + L"\\" + rel, out)) {
        if (!g_source) g_source = "mod folder";
        return true;
    }
    const std::wstring game = game_dir();
    if (game.empty()) return false;
    const std::wstring full = game + L"\\" + rel;
    const DWORD a = GetFileAttributesW(full.c_str());
    if (!read_game_file(full, *out)) return false;
    if (!g_source) g_source = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ? "game folder" : "a cat";
    return true;
}

uint32_t add_file(uint32_t h, const std::string& name, const std::vector<uint8_t>& bytes) {
    const std::string low = acmi::to_lower_ascii(name);
    h = content_fnv(h, reinterpret_cast<const uint8_t*>(low.data()), low.size());
    return content_fnv(h, bytes.data(), bytes.size());
}

constexpr uint32_t kSeed = 0x811C9DC5u;

// The hash of a main ini and the files it names (same folder) under any of keys; 0 if the main ini is missing.
uint32_t hash_object(const std::wstring& folder, const std::string& name, const std::vector<std::string>& keys,
                     acmi::Ini* main_out = nullptr, std::vector<std::pair<std::string, std::vector<uint8_t>>>* named = nullptr) {
    std::vector<uint8_t> bytes;
    if (!read_content_file(folder + L"\\" + widen_ansi(name) + L".ini", &bytes)) return 0;
    uint32_t h = add_file(kSeed, name + ".ini", bytes);
    const acmi::Ini ini = acmi::parse_ini(bytes.data(), bytes.size());
    for (const acmi::IniSection& s : ini.sections) {
        for (const auto& kv : s.items) {
            bool wanted = false;
            for (const auto& k : keys) wanted = wanted || kv.first == k;
            if (!wanted || !content_name_ok(kv.second)) continue;
            std::vector<uint8_t> f;
            const bool have = read_content_file(folder + L"\\" + widen_ansi(kv.second), &f);
            h = add_file(h, kv.second, have ? f : std::vector<uint8_t>{});
            if (named && have) named->emplace_back(acmi::to_lower_ascii(kv.first), std::move(f));
        }
    }
    if (main_out) *main_out = ini;
    return h ? h : 1u;
}

// The theatre's main ini and the files the sim reads from its [Terrain] section (tile and height data, targets,
// target types, movement, nations); maps, bitmaps and briefing text are left out.
uint32_t hash_theatre(const std::string& map, std::vector<std::string>* ground) {
    const std::wstring folder = L"Terrains\\" + widen_ansi(map);
    std::vector<uint8_t> bytes;
    if (!read_content_file(folder + L"\\" + widen_ansi(map) + L".ini", &bytes)) return 0;
    uint32_t h = add_file(kSeed, map + ".ini", bytes);
    const acmi::Ini th = acmi::parse_ini(bytes.data(), bytes.size());
    for (const char* key : {"DataFile", "TargetFile", "MovementFile", "NationsFile"}) {
        const std::string* f = th.get("Terrain", key);
        if (!f || !content_name_ok(*f)) continue;
        std::vector<uint8_t> b;
        h = add_file(h, *f, read_content_file(folder + L"\\" + widen_ansi(*f), &b) ? b : std::vector<uint8_t>{});
    }
    const std::string* targets = th.get("Terrain", "TargetFile");
    const std::string types = acmi::types_file_name(th, targets ? *targets : std::string());
    if (content_name_ok(types)) {
        std::vector<uint8_t> tf;
        const bool have = read_content_file(folder + L"\\" + widen_ansi(types), &tf);
        h = add_file(h, types, have ? tf : std::vector<uint8_t>{});
        if (have && ground) *ground = theatre_ground_objects(acmi::parse_ini(tf.data(), tf.size()));
    }
    return h ? h : 1u;
}

const std::vector<std::string> kAircraftFiles = {"aircraftdatafile", "loadoutfile", "avionicsdatafilename"};

// Weapons, racks and tanks are entries of Objects\Weapons\WeaponData.ini ([WeaponDataNNN] TypeName=...), not files
// of their own (a weapon's own small ini only names its model). Read once per listing or check.
struct WeaponIndex {
    bool loaded = false;
    const char* source = nullptr;  // where WeaponData.ini was found
    acmi::Ini ini;
};
thread_local WeaponIndex* g_weapons = nullptr;  // menu thread: set for the duration of mission_content / mission_content_check

uint32_t hash_weapon(const std::string& name) {
    WeaponIndex local;
    WeaponIndex& w = g_weapons ? *g_weapons : local;
    if (!w.loaded) {
        w.loaded = true;
        std::vector<uint8_t> bytes;
        const char* before = g_source;
        g_source = nullptr;
        if (read_content_file(L"Objects\\Weapons\\WeaponData.ini", &bytes)) w.ini = acmi::parse_ini(bytes.data(), bytes.size());
        w.source = g_source;
        g_source = before;
    }
    if (!g_source) g_source = w.source;
    const std::string want = acmi::to_lower_ascii(name);
    for (const acmi::IniSection& s : w.ini.sections) {
        bool match = false;
        for (const auto& kv : s.items)
            if (kv.first == "typename" && acmi::to_lower_ascii(kv.second) == want) match = true;
        if (!match) continue;
        uint32_t h = kSeed;
        for (const auto& kv : s.items) {
            const std::string line = kv.first + "=" + kv.second + "\n";
            h = content_fnv(h, reinterpret_cast<const uint8_t*>(line.data()), line.size());
        }
        return h ? h : 1u;
    }
    return 0;
}

constexpr int kPoolLine = 80, kPoolMax = 20000;

// SEH only (no C++ objects): the records as "category:name" lines into lines[kPoolMax][kPoolLine]. -1 when not as
// expected.
int read_ground_pool(char* lines) {
    HMODULE om = GetModuleHandleW(L"ObjectManager.dll");
    if (!om) return -1;
    const BYTE* mgr = static_cast<const BYTE*>(at_rva(om, 0x234d0));
    __try {
        if (*reinterpret_cast<void* const*>(mgr) != at_rva(om, 0x1d55c)) return -1;
        const uint32_t n = *reinterpret_cast<const uint32_t*>(mgr + 0x08);
        const BYTE* arr = *reinterpret_cast<const BYTE* const*>(mgr + 0x0c);
        if (!arr || n == 0 || n > 64) return -1;
        void* cls = nullptr;
        for (uint32_t i = 0; i < n && !cls; ++i) {
            const wchar_t* name = *reinterpret_cast<const wchar_t* const*>(arr + 0x10 * i + 0x08);
            if (name && _wcsnicmp(name, L"GroundObject", 12) == 0 && (name[12] == 0 || name[12] == L'.'))
                cls = *reinterpret_cast<void* const*>(arr + 0x10 * i);
        }
        if (!cls) return -1;
        using Count_t = int(__fastcall*)(void*, void*);
        using Get_t = void(__fastcall*)(void*, void*, int, void**);
        const int count = reinterpret_cast<Count_t>(vtable_slot(cls, 0))(cls, nullptr);
        if (count <= 0 || count > kPoolMax) return -1;
        for (int i = 0; i < count; ++i) {
            void* rec = nullptr;
            reinterpret_cast<Get_t>(vtable_slot(cls, 0x10 / 4))(cls, nullptr, i, &rec);
            if (!rec) return -1;
            const BYTE* r = static_cast<const BYTE*>(rec);
            const short category = *reinterpret_cast<const short*>(r + 0xbe4);
            const wchar_t* type = reinterpret_cast<const wchar_t*>(r + 0x04);
            if (category < 0 || category > 255) return -1;
            char* line = lines + static_cast<size_t>(i) * kPoolLine;
            int k = std::snprintf(line, kPoolLine, "%d:", static_cast<int>(category));
            for (int c = 0; c < 64 && type[c] && k < kPoolLine - 1; ++c) {
                if (type[c] < 0x20 || type[c] > 0x7e) return -1;  // not a type record as analysed
                line[k++] = static_cast<char>(type[c] >= 'A' && type[c] <= 'Z' ? type[c] + 32 : type[c]);
            }
            line[k] = 0;
            if (k <= 2) return -1;
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The ground object types the game picks from (by category, at generate and at the flight's start): the type
// records of the GroundObject class, read from ObjectManager's class table (see lazy-sf2-recon: recon-notes §20,
// §28.7d). The pool hash covers each record's category and name. 0 when anything is not as expected (fails closed).
thread_local const SharedTypes* g_pool_filter = nullptr;  // a filtered mission: only the shared types are in its pool

uint32_t ground_pool_hash() {
    if (!is_expected_build(GetModuleHandleW(L"ObjectManager.dll"), 0x51d21fa8, 0x28000, "ObjectManager.dll")) return 0;
    std::vector<char> buf(static_cast<size_t>(kPoolMax) * kPoolLine, 0);
    const int n = read_ground_pool(buf.data());
    if (n <= 0) return 0;
    std::vector<std::string> items;
    items.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const char* line = buf.data() + static_cast<size_t>(i) * kPoolLine;
        const char* colon = std::strchr(line, ':');
        if (g_pool_filter && (!colon || !shared_has(g_pool_filter->ground, colon + 1))) continue;
        items.emplace_back(line);
    }
    std::sort(items.begin(), items.end());
    uint32_t h = kSeed;
    for (const auto& s : items) h = content_fnv(h, reinterpret_cast<const uint8_t*>(s.c_str()), s.size() + 1);
    return h ? h : 1u;
}

}  // namespace

uint32_t content_hash_in(char kind, const std::wstring& dir, const std::string& name) {
    g_source = nullptr;
    if (!content_name_ok(name) || dir.empty()) return 0;
    if (kind == 'A') return hash_object(dir, name, kAircraftFiles);
    if (kind == 'G') return hash_object(dir, name, {});
    return 0;
}

uint32_t content_entry_hash(char kind, const std::string& name, const char** source) {
    g_source = nullptr;
    struct Report {
        const char** out;
        ~Report() {
            if (out) *out = g_source ? g_source : "nowhere";
        }
    } report{source};
    if (!content_name_ok(name)) return 0;
    switch (kind) {
        case 'T': return hash_theatre(name, nullptr);
        case 'A': return hash_object(L"Objects\\Aircraft\\" + widen_ansi(name), name, kAircraftFiles);
        case 'W': return hash_weapon(name);
        case 'P': return ground_pool_hash();
        case 'G': return hash_object(L"Objects\\GroundObject\\" + widen_ansi(name), name, {});
        default: return 0;
    }
}

std::vector<ContentEntry> mission_content(const std::vector<uint8_t>& msn, const SharedTypes* shared) {
    g_pool_filter = shared && !shared->empty() ? shared : nullptr;
    struct ClearPool {
        ~ClearPool() { g_pool_filter = nullptr; }
    } clear_pool;
    WeaponIndex weapons_index;
    g_weapons = &weapons_index;
    struct Clear {
        ~Clear() { g_weapons = nullptr; }
    } clear;
    std::vector<ContentEntry> out;
    const MissionUses uses = mission_uses(acmi::parse_ini(msn.data(), msn.size()));
    auto have = [&](char kind, const std::string& n) {
        for (const auto& e : out)
            if (e.kind == kind && acmi::to_lower_ascii(e.name) == acmi::to_lower_ascii(n)) return true;
        return false;
    };
    // The theatre's types file names ground object categories (SAMLauncher, AAA), not object folders: the objects are
    // picked at run time, so they are compared through the theatre entry (its types file) only.
    if (!uses.theatre.empty()) out.push_back({'T', uses.theatre, hash_theatre(uses.theatre, nullptr)});
    std::vector<std::string> weapons, dropped = uses.dropped;
    for (const auto& [type, loadout] : uses.aircraft) {
        acmi::Ini main;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> named;
        const uint32_t h = hash_object(L"Objects\\Aircraft\\" + widen_ansi(type), type, kAircraftFiles, &main, &named);
        if (!have('A', type)) out.push_back({'A', type, h});
        for (const auto& [key, bytes] : named) {
            if (key != "loadoutfile") continue;
            bool no_section = false;
            const auto ws = loadout_weapons(acmi::parse_ini(bytes.data(), bytes.size()), loadout, &dropped, &no_section);
            if (no_section && !loadout.empty())  // the header's player type names no loadout: its flight does
                LOG("lobby: %s has no loadout \"%s\" and no [Default] one; its weapons are not compared", type.c_str(),
                    loadout.c_str());
            for (const auto& w : ws) {
                bool seen = false;
                for (const auto& x : weapons) seen = seen || acmi::to_lower_ascii(x) == acmi::to_lower_ascii(w);
                if (!seen) weapons.push_back(w);
            }
        }
    }
    for (const auto& w : weapons) out.push_back({'W', w, content_entry_hash('W', w, nullptr)});
    out.push_back({'P', "GroundObjects", ground_pool_hash()});
    if (out.back().hash == 0) LOG("lobby: the ground object pool could not be read here; joined pilots will be told");
    size_t missing = 0;
    std::string missing_names;
    for (const auto& e : out) {
        if (e.hash) continue;
        ++missing;
        missing_names += (missing_names.empty() ? "" : ", ") + std::string(1, e.kind) + ":" + e.name;
    }
    if (missing)
        LOG("lobby: not found on this PC, so not compared: %s", missing_names.c_str());
    if (!dropped.empty()) {
        std::string names;
        for (size_t i = 0; i < dropped.size() && i < 6; ++i) names += (i ? ", " : "") + dropped[i];
        LOG("lobby: %zu name(s) in the mission cannot be listed (control characters, path characters or too long), so "
            "they are not compared: %s%s",
            dropped.size(), names.c_str(), dropped.size() > 6 ? ", ..." : "");
    }
    LOG("lobby: the mission uses %zu content entries (theatre %s, %zu aircraft types, %zu weapons and racks); %zu not "
        "found here",
        out.size(), uses.theatre.c_str(), uses.aircraft.size(), weapons.size(), missing);
    return out;
}

std::string mission_content_check(const std::string& sidecar_text) {
    WeaponIndex weapons_index;
    g_weapons = &weapons_index;
    struct Clear {
        ~Clear() { g_weapons = nullptr; }
    } clear;
    std::vector<ContentEntry> host = parse_content(sidecar_text);
    if (host.empty()) return {};  // a host that sends no list: nothing to compare
    const SharedTypes shared = parse_shared(sidecar_text);
    g_pool_filter = shared.empty() ? nullptr : &shared;
    if (!shared.empty()) {
        // Generated from shared content: this PC's records must give the host's sequence (same names, hashes, order),
        // or the generator's and the start's picks differ from one seed.
        std::vector<ContentEntry> inv;
        std::string why;
        if (!gen_inventory(&inv, &why)) return std::string(1, kContentNotReady);
        if (inv.empty()) return "this PC's types could not be listed (" + why + ")";
        if (const std::string order = verify_shared_order(shared, inv); !order.empty()) {
            LOG("lobby: the host's shared types do not match this PC's: %s", order.c_str());
            return "shared types: " + order;
        }
    }
    struct ClearPool {
        ~ClearPool() { g_pool_filter = nullptr; }
    } clear_pool;
    if (host.size() > kMaxContentEntries)
        return "the host's list is too long to check (" + std::to_string(host.size()) + " entries)";
    std::vector<ContentEntry> mine;
    mine.reserve(host.size());
    std::string sources;  // where this PC's differing or missing entries came from, for tracing a mismatch to a mod
    for (const auto& e : host) {
        const char* from = nullptr;
        mine.push_back({e.kind, e.name, content_entry_hash(e.kind, e.name, &from)});
        if (e.hash != 0 && mine.back().hash != e.hash)
            sources += (sources.empty() ? "" : ", ") + std::string(1, e.kind) + ":" + e.name + " (" +
                       (mine.back().hash ? from : "not found") + ")";
    }
    if (!sources.empty()) LOG("lobby: this PC's copies that differ from the host's: %s", sources.c_str());
    return content_diff(host, mine);
}

}  // namespace LazyWorks::SF2Patch::hooks
