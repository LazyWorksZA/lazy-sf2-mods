#pragma once
// Generating and flying only what every PC in a lobby session has (owner's option B): the aircraft and ground object
// types all inventories share. The pure part is here (unit tested); the detours on SingleMission.dll's availability
// functions and the record walks are in gen_filter.cpp. See lazy-sf2-recon: recon-notes §28.7d.
#include <algorithm>
#include <string>
#include <vector>

#include "acmi/ini.h"
#include "hooks/mission_content.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The type names a filtered mission may use (lower case, sorted): kind 'A' aircraft, 'G' ground objects.
/// </summary>
struct SharedTypes {
    std::vector<std::string> aircraft, ground;
    bool empty() const { return aircraft.empty() && ground.empty(); }
};

inline void sort_unique(std::vector<std::string>* v) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
}

/// <summary>The aircraft and ground object entries every inventory has with the same hash (an entry with hash 0 is
/// not had). The first inventory is the host's.</summary>
inline SharedTypes intersect_inventories(const std::vector<std::vector<ContentEntry>>& inventories) {
    SharedTypes out;
    if (inventories.empty()) return out;
    for (const ContentEntry& e : inventories[0]) {
        if ((e.kind != 'A' && e.kind != 'G') || e.hash == 0) continue;
        const std::string name = acmi::to_lower_ascii(e.name);
        bool all = true;
        for (size_t i = 1; i < inventories.size() && all; ++i) {
            bool found = false;
            for (const ContentEntry& o : inventories[i])
                if (o.kind == e.kind && o.hash == e.hash && acmi::to_lower_ascii(o.name) == name) found = true;
            all = found;
        }
        if (all) (e.kind == 'A' ? out.aircraft : out.ground).push_back(name);
    }
    sort_unique(&out.aircraft);
    sort_unique(&out.ground);
    return out;
}

/// <summary>Sidecar lines "shared=A:name" / "shared=G:name": the set a mission was generated with, applied by every PC
/// at its load and start.</summary>
inline std::string format_shared(const SharedTypes& s) {
    std::string out;
    for (const auto& n : s.aircraft) out += "shared=A:" + n + "\n";
    for (const auto& n : s.ground) out += "shared=G:" + n + "\n";
    return out;
}

/// <summary>The shared lines of a sidecar (empty: the mission was not generated with a filter).</summary>
inline SharedTypes parse_shared(const std::string& text) {
    SharedTypes s;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 10 || line.rfind("shared=", 0) != 0 || line[8] != ':') continue;
        const std::string name = acmi::to_lower_ascii(line.substr(9));
        if (!content_name_ok(name)) continue;
        if (line[7] == 'A') s.aircraft.push_back(name);
        if (line[7] == 'G') s.ground.push_back(name);
    }
    sort_unique(&s.aircraft);
    sort_unique(&s.ground);
    return s;
}

/// <summary>Whether name (any case) is in a sorted lower-case list.</summary>
inline bool shared_has(const std::vector<std::string>& sorted, const std::string& name) {
    return std::binary_search(sorted.begin(), sorted.end(), acmi::to_lower_ascii(name));
}

// --- gen_filter.cpp: the detours and the record walks. ---

/// <summary>Menu thread: from now on the generator, a mission load and the flight's start pick only types in s (the
/// detours return "not available" for the others). False, with *why, when the type records are not as analysed:
/// nothing is filtered then (fails closed: the caller must not fly the mission as filtered).</summary>
bool gen_filter_apply(const SharedTypes& s, std::string* why);
/// <summary>Any thread (slow: it reads every type's files): this PC's aircraft and ground object types, each hashed
/// from the folder its type record names (as the game found it). Empty, with *why, when the records are not as
/// analysed.</summary>
std::vector<ContentEntry> gen_local_inventory(std::string* why);
/// <summary>Menu thread: builds this PC's inventory on a worker thread, once per game run (types do not change while
/// the game runs). Later calls do nothing.</summary>
void gen_inventory_start();
/// <summary>Any thread: the inventory once built (true), else false; *why when it could not be built.</summary>
bool gen_inventory(std::vector<ContentEntry>* out, std::string* why);
/// <summary>Back to stock picking.</summary>
void gen_filter_clear();
bool gen_filter_active();
/// <summary>Module watch: hooks SingleMission.dll (per image, stamp and prologues checked).</summary>
void gen_filter_on_modules_loaded();

}  // namespace LazyWorks::SF2Patch::hooks
