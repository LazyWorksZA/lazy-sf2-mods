#pragma once
// What a shared mission uses (the theatre, every aircraft type, the weapons in their loadouts, the theatre's ground
// object types), each with a hash of its defining files, so a joining PC can check it has the same (L4). The pure part
// is here (unit tested); the file reads are in mission_content.cpp.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "acmi/ini.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>One thing a mission uses. kind: 'T' theatre, 'A' aircraft, 'W' weapon, 'G' ground object, 'P' the pool
/// of ground object types the game picks from at run time (by category). hash 0: the PC has none ('P': could not be
/// read, which never counts as a match).</summary>
struct ContentEntry {
    char kind = 0;
    std::string name;
    uint32_t hash = 0;
};

/// <summary>A name taken from a game file, usable in a path and a sidecar line: 1..64 printable ASCII characters
/// (spaces inside, as folder names may have) without path separators, ':', '=' or "..".</summary>
inline bool content_name_ok(const std::string& n) {
    if (n.empty() || n.size() > 64 || n.find("..") != std::string::npos || n.front() == ' ' || n.back() == ' ')
        return false;
    for (char c : n)
        if (c < 0x20 || c > 0x7e || c == '\\' || c == '/' || c == ':' || c == '=') return false;
    return true;
}

/// <summary>The most content lines a client checks; a longer list is refused (it would cost the menu thread too
/// much).</summary>
inline constexpr size_t kMaxContentEntries = 512;

/// <summary>The mission file's theatre and its flights' aircraft types with their loadout names (in file order,
/// repeated types kept once per loadout).</summary>
struct MissionUses {
    std::string theatre;
    std::vector<std::pair<std::string, std::string>> aircraft;  // type, loadout section (may be empty)
    std::vector<std::string> dropped;                           // names that cannot be used (logged by the caller)
};

inline MissionUses mission_uses(const acmi::Ini& msn) {
    MissionUses u;
    if (const std::string* m = msn.get("MissionHeader", "MissionMap")) {
        if (content_name_ok(*m))
            u.theatre = *m;
        else
            u.dropped.push_back(*m);
    }
    auto add = [&](const std::string& type, const std::string& loadout) {
        if (!content_name_ok(type)) {
            if (!type.empty()) u.dropped.push_back(type);
            return;
        }
        for (const auto& a : u.aircraft)
            if (acmi::to_lower_ascii(a.first) == acmi::to_lower_ascii(type) && a.second == loadout) return;
        u.aircraft.emplace_back(type, loadout);
    };
    if (const std::string* t = msn.get("MissionHeader", "AircraftType")) add(*t, "");
    for (const acmi::IniSection& s : msn.sections) {
        if (s.name.rfind("aircraftmission", 0) != 0) continue;
        std::string type, loadout;
        for (const auto& kv : s.items) {
            if (kv.first == "aircrafttype") type = kv.second;
            if (kv.first == "loadout") loadout = kv.second;
        }
        add(type, loadout);
    }
    return u;
}

/// <summary>The weapons and racks a loadout section of an aircraft's loadout ini names (Loadout[nn].WeaponType and
/// .RackType), each once. Without that section (none named, or not in the file) the [Default] section, else none:
/// listing every section would name weapons nobody flies and refuse a smaller install (*no_section is set then).
/// </summary>
inline std::vector<std::string> loadout_weapons(const acmi::Ini& loadout_ini, const std::string& section,
                                                std::vector<std::string>* dropped = nullptr, bool* no_section = nullptr) {
    std::vector<std::string> out;
    std::vector<const acmi::IniSection*> from;
    if (no_section) *no_section = false;
    if (const acmi::IniSection* s = section.empty() ? nullptr : loadout_ini.find(section)) {
        from.push_back(s);
    } else if (const acmi::IniSection* d = loadout_ini.find("Default")) {
        from.push_back(d);
    } else if (no_section) {
        *no_section = true;
    }
    for (const acmi::IniSection* s : from) {
        for (const auto& kv : s->items) {
            const size_t dot = kv.first.rfind('.');
            if (dot == std::string::npos) continue;
            const std::string field = kv.first.substr(dot + 1);
            if (field != "weapontype" && field != "racktype") continue;
            if (!content_name_ok(kv.second)) {
                if (dropped && !kv.second.empty()) dropped->push_back(kv.second);
                continue;
            }
            bool seen = false;
            for (const auto& w : out) seen = seen || acmi::to_lower_ascii(w) == acmi::to_lower_ascii(kv.second);
            if (!seen) out.push_back(kv.second);
        }
    }
    return out;
}

/// <summary>The ground object types a theatre's types ini uses (UseGroundObject=TRUE: Name is the object's folder).
/// </summary>
inline std::vector<std::string> theatre_ground_objects(const acmi::Ini& types) {
    std::vector<std::string> out;
    for (const acmi::IniSection& s : types.sections) {
        std::string name;
        bool ground = false;
        for (const auto& kv : s.items) {
            if (kv.first == "name") name = kv.second;
            if (kv.first == "usegroundobject") ground = acmi::to_lower_ascii(kv.second) == "true";
        }
        if (!ground || !content_name_ok(name)) continue;
        bool seen = false;
        for (const auto& g : out) seen = seen || acmi::to_lower_ascii(g) == acmi::to_lower_ascii(name);
        if (!seen) out.push_back(name);
    }
    return out;
}

/// <summary>FNV-1a over bytes, continuing from h.</summary>
inline uint32_t content_fnv(uint32_t h, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h ^= d[i];
        h *= 0x01000193u;
    }
    return h;
}

/// <summary>Sidecar lines: "content=A:F-105D:1a2b3c4d" (an old client ignores them).</summary>
inline std::string format_content(const std::vector<ContentEntry>& es) {
    std::string out;
    char line[96];
    for (const ContentEntry& e : es) {
        if (!content_name_ok(e.name)) continue;
        std::snprintf(line, sizeof(line), "content=%c:%s:%08lx\n", e.kind, e.name.c_str(), static_cast<unsigned long>(e.hash));
        out += line;
    }
    return out;
}

/// <summary>The content lines of a sidecar text; malformed ones are skipped.</summary>
inline std::vector<ContentEntry> parse_content(const std::string& text) {
    std::vector<ContentEntry> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("content=", 0) != 0) continue;
        const std::string v = line.substr(8);
        const size_t last = v.rfind(':');
        if (v.size() < 5 || v[1] != ':' || last == std::string::npos || last < 3) continue;
        const char kind = v[0];
        if (kind != 'T' && kind != 'A' && kind != 'W' && kind != 'G' && kind != 'P') continue;
        const std::string name = v.substr(2, last - 2), hex = v.substr(last + 1);
        if (!content_name_ok(name) || hex.empty() || hex.size() > 8) continue;
        char* end = nullptr;
        const unsigned long h = std::strtoul(hex.c_str(), &end, 16);
        if (*end) continue;
        out.push_back({kind, name, static_cast<uint32_t>(h)});
    }
    return out;
}

/// <summary>"missing: F-4E_67, AIM-9B; differs: AGM-12B": the host's entries this PC lacks (hash 0 here) or has
/// otherwise, at most max_names named (then "and N more"). Empty when all match. An entry the host lacks (hash 0)
/// matches whatever this PC has: the host's flight never uses it.</summary>
inline std::string content_diff(const std::vector<ContentEntry>& host, const std::vector<ContentEntry>& local,
                                size_t max_names = 4) {
    std::vector<std::string> missing, differs;
    for (const ContentEntry& h : host) {
        uint32_t mine = 0;
        for (const ContentEntry& l : local)
            if (l.kind == h.kind && acmi::to_lower_ascii(l.name) == acmi::to_lower_ascii(h.name)) mine = l.hash;
        if (h.kind == 'P') {
            // The ground object pool: the same set or the units placed from one seed differ; unreadable never matches.
            if (h.hash != 0 && mine == h.hash) continue;
            differs.push_back(h.hash == 0 || mine == 0 ? "ground objects (could not be compared)" : "ground objects");
            continue;
        }
        if (mine == h.hash || h.hash == 0) continue;  // the host lacks it too: its flight never uses it
        (mine == 0 ? missing : differs).push_back(h.name);
    }
    std::string out;
    size_t named = 0, more = 0;
    auto list = [&](const char* label, const std::vector<std::string>& v) {
        if (v.empty()) return;
        if (named == max_names) {
            more += v.size();
            return;
        }
        if (!out.empty()) out += "; ";
        out += label;
        bool first = true;
        for (const auto& n : v) {
            if (named == max_names) {
                ++more;
                continue;
            }
            out += first ? " " : ", ";
            out += n;
            first = false;
            ++named;
        }
    };
    list("missing:", missing);
    list("differs:", differs);
    if (more) out += "; and " + std::to_string(more) + " more";
    return out;
}

}  // namespace LazyWorks::SF2Patch::hooks
