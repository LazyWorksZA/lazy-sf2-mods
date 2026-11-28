#pragma once
// Generating and flying only what every PC in a lobby session has (owner's option B): the aircraft and ground object
// types all inventories share. The pure part is here (unit tested); the detours on SingleMission.dll's availability
// functions and the record walks are in gen_filter.cpp. See lazy-sf2-recon: recon-notes §28.7d.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "acmi/ini.h"
#include "hooks/mission_content.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The types a filtered mission may use: `ordered` in the host's type-record order with the hashes (the
/// generator walks records in that order, so every PC must have the same sequence), and the names sorted for lookups
/// (lower case): kind 'A' aircraft, 'G' ground objects.</summary>
struct SharedTypes {
    std::vector<ContentEntry> ordered;
    std::vector<std::string> aircraft, ground;
    bool empty() const { return ordered.empty(); }
};

inline void sort_unique(std::vector<std::string>* v) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
}

inline void index_names(SharedTypes* s) {
    s->aircraft.clear();
    s->ground.clear();
    for (const ContentEntry& e : s->ordered) (e.kind == 'A' ? s->aircraft : s->ground).push_back(acmi::to_lower_ascii(e.name));
    sort_unique(&s->aircraft);
    sort_unique(&s->ground);
}

/// <summary>The aircraft and ground object entries every inventory has with the same hash (an entry with hash 0 is
/// not had), in the order of the first inventory (the host's, built in its record order).</summary>
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
        if (all) out.ordered.push_back({e.kind, name, e.hash});
    }
    index_names(&out);
    return out;
}

/// <summary>Sidecar lines "shared=K:name:hash", in the host's record order: the set a mission was generated with,
/// checked and applied by every PC at its load and start.</summary>
inline std::string format_shared(const SharedTypes& s) {
    std::string out;
    char line[96];
    for (const ContentEntry& e : s.ordered) {
        std::snprintf(line, sizeof(line), "shared=%c:%s:%08lx\n", e.kind, e.name.c_str(), static_cast<unsigned long>(e.hash));
        out += line;
    }
    return out;
}

/// <summary>The shared lines of a sidecar, in order (empty: the mission was not generated with a filter).
/// Malformed lines are skipped.</summary>
inline SharedTypes parse_shared(const std::string& text) {
    SharedTypes s;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 12 || line.rfind("shared=", 0) != 0 || line[8] != ':' || (line[7] != 'A' && line[7] != 'G')) continue;
        const size_t last = line.rfind(':');
        if (last <= 9) continue;
        const std::string name = acmi::to_lower_ascii(line.substr(9, last - 9)), hex = line.substr(last + 1);
        char* end = nullptr;
        const unsigned long h = std::strtoul(hex.c_str(), &end, 16);
        if (!content_name_ok(name) || hex.empty() || hex.size() > 8 || *end || h == 0) continue;
        s.ordered.push_back({line[7], name, static_cast<uint32_t>(h)});
    }
    index_names(&s);
    return s;
}

/// <summary>Whether this PC's inventory (in its record order) holds exactly the host's shared sequence: the same
/// names with the same hashes in the same order, per kind (its other types are filtered out). Empty when it does,
/// else what differs first.</summary>
inline std::string verify_shared_order(const SharedTypes& host, const std::vector<ContentEntry>& mine) {
    for (const char kind : {'A', 'G'}) {
        const std::vector<std::string>& names = kind == 'A' ? host.aircraft : host.ground;
        std::vector<const ContentEntry*> want, have;
        for (const ContentEntry& e : host.ordered)
            if (e.kind == kind) want.push_back(&e);
        for (const ContentEntry& e : mine)
            if (e.kind == kind && std::binary_search(names.begin(), names.end(), acmi::to_lower_ascii(e.name))) have.push_back(&e);
        for (size_t i = 0; i < want.size(); ++i) {
            if (i >= have.size()) return want[i]->name + " is missing";
            if (acmi::to_lower_ascii(have[i]->name) != want[i]->name) {
                bool elsewhere = false;
                for (const ContentEntry* h : have) elsewhere = elsewhere || acmi::to_lower_ascii(h->name) == want[i]->name;
                return elsewhere ? "the type order differs at " + want[i]->name : want[i]->name + " is missing";
            }
            if (have[i]->hash != want[i]->hash) return want[i]->name + " differs";
        }
        if (have.size() > want.size()) return have[want.size()]->name + " is listed twice";
    }
    return {};
}

/// <summary>Whether name (any case) is in a sorted lower-case list.</summary>
inline bool shared_has(const std::vector<std::string>& sorted, const std::string& name) {
    return std::binary_search(sorted.begin(), sorted.end(), acmi::to_lower_ascii(name));
}

/// <summary>A model of SingleMission's ground pickers (+0x23d10, +0x23b80), for tests: among the records of
/// `category` (category[i]), the weighted ones (weight[i] > 0) are the candidates; without any, when `fallback` is set,
/// the eligible record (eligible[i]) with the smallest |year - service_year[i]|, the first on a tie; -1 if none. Hidden
/// records (the filter's unshared ones) have their category swapped out, so they are never candidates.</summary>
inline int model_ground_fallback(const std::vector<short>& category, const std::vector<int>& weight,
                                 const std::vector<bool>& eligible, const std::vector<int>& service_year,
                                 const std::vector<bool>& hidden, short want, int year, bool fallback,
                                 std::vector<int>* candidates) {
    int best = -1, best_d = -1;
    for (size_t i = 0; i < category.size(); ++i) {
        if (hidden[i] || category[i] != want) continue;
        if (weight[i] > 0) {
            candidates->push_back(static_cast<int>(i));
            continue;
        }
        if (!eligible[i]) continue;
        const int d = year > service_year[i] ? year - service_year[i] : service_year[i] - year;
        if (best_d == -1 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return candidates->empty() && fallback ? best : -1;
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
