// The files behind a mission's content list (mission_content.h): read from the player's own folder first (mods),
// then through the exe's file manager (loose files in the game folder, then the cats), as the game finds them.
#include <windows.h>

#include <string>
#include <vector>

#include "acmi/airfields.h"
#include "config.h"
#include "hooks/game_files.h"
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

// rel: a path under the game folder, as "Objects\Aircraft\F-4E\F-4E.ini".
bool read_content_file(const std::wstring& rel, std::vector<uint8_t>* out) {
    const std::wstring user = config::game_data_dir();
    if (!user.empty() && read_loose(user + L"\\" + rel, out)) return true;
    const std::wstring game = game_dir();
    return !game.empty() && read_game_file(game + L"\\" + rel, *out);
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
    acmi::Ini ini;
};
WeaponIndex* g_weapons = nullptr;  // menu thread: set for the duration of mission_content / mission_content_check

uint32_t hash_weapon(const std::string& name) {
    WeaponIndex local;
    WeaponIndex& w = g_weapons ? *g_weapons : local;
    if (!w.loaded) {
        w.loaded = true;
        std::vector<uint8_t> bytes;
        if (read_content_file(L"Objects\\Weapons\\WeaponData.ini", &bytes)) w.ini = acmi::parse_ini(bytes.data(), bytes.size());
    }
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

}  // namespace

uint32_t content_entry_hash(char kind, const std::string& name) {
    if (!content_name_ok(name)) return 0;
    switch (kind) {
        case 'T': return hash_theatre(name, nullptr);
        case 'A': return hash_object(L"Objects\\Aircraft\\" + widen_ansi(name), name, kAircraftFiles);
        case 'W': return hash_weapon(name);
        case 'G': return hash_object(L"Objects\\GroundObject\\" + widen_ansi(name), name, {});
        default: return 0;
    }
}

std::vector<ContentEntry> mission_content(const std::vector<uint8_t>& msn) {
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
    for (const auto& w : weapons) out.push_back({'W', w, content_entry_hash('W', w)});
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
    if (host.size() > kMaxContentEntries)
        return "the host's list is too long to check (" + std::to_string(host.size()) + " entries)";
    std::vector<ContentEntry> mine;
    mine.reserve(host.size());
    for (const auto& e : host) mine.push_back({e.kind, e.name, content_entry_hash(e.kind, e.name)});
    return content_diff(host, mine);
}

}  // namespace LazyWorks::SF2Patch::hooks
