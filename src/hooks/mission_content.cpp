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
thread_local bool g_trace = false;  // log every file read, its source, size and hash (the theatre entry)

// Where rel was found (nullptr: nowhere); rel is a path under the game folder, as "Objects\Aircraft\F-4E\F-4E.ini",
// or a type record's own (absolute) folder.
const char* find_content_file(const std::wstring& rel, std::vector<uint8_t>* out) {
    if (rel.size() > 2 && rel[1] == L':') {  // that loose file, else the cats
        const DWORD a = GetFileAttributesW(rel.c_str());
        const bool loose = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        if (!(loose ? read_loose(rel, out) : read_game_file(rel, *out))) return nullptr;
        const std::wstring user = config::game_data_dir();
        return !loose ? "a cat"
               : !user.empty() && _wcsnicmp(rel.c_str(), user.c_str(), user.size()) == 0 ? "mod folder"
                                                                                       : "game folder";
    }
    const std::wstring user = config::game_data_dir();
    if (!user.empty() && read_loose(user + L"\\" + rel, out)) return "mod folder";
    const std::wstring game = game_dir();
    if (game.empty()) return nullptr;
    const std::wstring full = game + L"\\" + rel;
    const DWORD a = GetFileAttributesW(full.c_str());
    if (!read_game_file(full, *out)) return nullptr;
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ? "game folder" : "a cat";
}

bool read_content_file(const std::wstring& rel, std::vector<uint8_t>* out) {
    const char* from = find_content_file(rel, out);
    if (g_trace) {
        if (from)
            LOG("lobby: content file %ls: %s, %zu bytes, hash 0x%08lx", rel.c_str(), from, out->size(),
                static_cast<unsigned long>(content_fnv(0x811C9DC5u, out->data(), out->size())));
        else
            LOG("lobby: content file %ls: not found (mod folder, game folder, mounted cats)", rel.c_str());
    }
    if (from && !g_source) g_source = from;
    return from != nullptr;
}

/// <summary>The theatre entry's hash with every file it reads logged (once per listing or check).</summary>
uint32_t hash_theatre(const std::string& map, std::vector<std::string>* ground);
uint32_t traced_theatre(const std::string& map) {
    g_trace = true;
    const uint32_t h = hash_theatre(map, nullptr);
    g_trace = false;
    LOG("lobby: theatre %s: entry hash 0x%08lx", map.c_str(), static_cast<unsigned long>(h));
    return h;
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

// The ground object types the game picks from (by category, at generate and at the flight's start): the records of
// the verified GroundObject type manager (gen_filter.cpp; menu thread only). The pool hash covers each record's
// category and name. 0 when they are not as analysed (fails closed: never a match).
thread_local const SharedTypes* g_pool_filter = nullptr;  // a filtered mission: only the shared types are in its pool

uint32_t ground_pool_hash() {
    std::vector<TypeRecord> types;
    std::string why;
    if (!gen_walk_types(&types, &why)) return 0;
    std::vector<std::string> items;
    for (const TypeRecord& r : types) {
        if (r.kind != 'G') continue;
        if (g_pool_filter && !shared_has(g_pool_filter->ground, r.name)) continue;
        items.push_back(std::to_string(r.category) + ":" + acmi::to_lower_ascii(r.name));
    }
    if (items.empty()) return 0;
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
    if (!uses.theatre.empty()) out.push_back({'T', uses.theatre, traced_theatre(uses.theatre)});
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
        if (e.kind == 'T') g_trace = true;
        mine.push_back({e.kind, e.name, content_entry_hash(e.kind, e.name, &from)});
        if (e.kind == 'T') {
            g_trace = false;
            LOG("lobby: theatre %s: entry hash 0x%08lx here, 0x%08lx at the host", e.name.c_str(),
                static_cast<unsigned long>(mine.back().hash), static_cast<unsigned long>(e.hash));
        }
        if (e.hash != 0 && mine.back().hash != e.hash)
            sources += (sources.empty() ? "" : ", ") + std::string(1, e.kind) + ":" + e.name + " (" +
                       (mine.back().hash ? from : "not found") + ")";
    }
    if (!sources.empty()) LOG("lobby: this PC's copies that differ from the host's: %s", sources.c_str());
    return content_diff(host, mine);
}

}  // namespace LazyWorks::SF2Patch::hooks
