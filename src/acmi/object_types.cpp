#include "acmi/object_types.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "acmi/tacview_core.h"

namespace LazyWorks::SF2Patch::acmi {

namespace {

using Words = std::vector<std::string>;

/// Lowercase alphanumeric words: "SA-13 Launcher" gives sa, 13, launcher.
Words words_of(std::string_view s) {
    Words out;
    std::string cur;
    for (char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalnum(u)) {
            cur += static_cast<char>(std::tolower(u));
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool word_is(const std::string& w, const char* key) {
    const std::string k(key);
    return w == k || (w.size() == k.size() + 1 && w.back() == 's' && w.compare(0, k.size(), k) == 0);
}

/// True when any key is a whole word of the name; a key of several words ("fuel tank") must match consecutive words.
bool has_any(const Words& ws, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const Words kw = words_of(key);
        if (kw.empty() || kw.size() > ws.size()) continue;
        for (size_t i = 0; i + kw.size() <= ws.size(); ++i) {
            bool ok = true;
            for (size_t j = 0; j < kw.size() && ok; ++j) ok = word_is(ws[i + j], kw[j].c_str());
            if (ok) return true;
        }
    }
    return false;
}

/// "sa-2", "sa-13": a surface-to-air missile designation.
bool has_sa_designation(const Words& ws) {
    for (size_t i = 0; i + 1 < ws.size(); ++i)
        if (ws[i] == "sa" && std::all_of(ws[i + 1].begin(), ws[i + 1].end(), [](unsigned char c) { return std::isdigit(c); }))
            return true;
    return false;
}

bool is_anti_aircraft(const Words& n) {
    return has_any(n, {"aaa", "flak", "zsu", "anti-air", "antiair", "aa gun", "sam", "missile site"}) || has_sa_designation(n);
}

bool is_storage(const Words& n) {
    return has_any(n, {"fuel tank", "storage tank", "oil tank", "tank farm", "fuel depot", "fuel dump", "fuel storage",
                       "oil storage", "silo"});
}

const char* vehicle_tags(const Words& n) {
    if (is_anti_aircraft(n)) return "Ground+AntiAircraft";
    if (has_any(n, {"truck", "lorry", "convoy", "supply", "ammo", "tanker", "bus"})) return "Ground+Medium+Vehicle";
    if (has_any(n, {"apc", "armored", "armoured", "personnel carrier", "ifv", "bmp", "btr"})) return "Ground+Heavy+Armor+Vehicle";
    if (has_any(n, {"jeep", "car", "pickup", "van", "staff", "scout", "buggy", "motorcycle", "utility"}))
        return "Ground+Light+Vehicle";
    if (has_any(n, {"infantry", "soldier", "troop", "squad", "platoon"})) return "Ground+Light+Human+Infantry";
    if (has_any(n, {"tank", "mbt"})) return "Ground+Heavy+Armor+Vehicle+Tank";
    if (has_any(n, {"howitzer", "artillery", "mortar", "mlrs", "rocket launcher", "self-propelled", "gun"}))
        return "Ground+Heavy+Armor+Vehicle";
    return "Ground+Vehicle";
}

const char* ship_tags(const Words& n) {
    if (has_any(n, {"carrier", "cvn", "cva"})) return "Sea+Watercraft+AircraftCarrier";
    if (has_any(n, {"submarine"})) return "Sea+Watercraft+Submarine";
    if (has_any(n, {"destroyer", "cruiser", "frigate", "corvette", "battleship", "gunboat", "patrol", "missile boat",
                    "warship", "minesweeper", "landing ship"}))
        return "Sea+Watercraft+Warship";
    return "Sea+Watercraft";
}

const char* static_tags(const Words& n) {
    if (is_storage(n)) return "Ground+Static+Container";
    if (is_anti_aircraft(n) || has_any(n, {"radar"})) return "Ground+AntiAircraft";
    return "Ground+Static+Building";
}

}  // namespace

bool is_storage_category(std::string_view category) {
    return has_any(words_of(category), {"fuel", "storage", "oil", "pol"});
}

const char* ground_type_tags(uint8_t cls, std::string_view name) {
    const Words n = words_of(name);
    switch (cls) {
        case TacviewRec::kShip: return ship_tags(n);
        case TacviewRec::kGroundVehicle: return vehicle_tags(n);
        case TacviewRec::kBridge: return "Ground+Static+Building";  // the SDK defines no Bridge tag
        case TacviewRec::kStorage: return "Ground+Static+Container";
        default: return static_tags(n);
    }
}

}  // namespace LazyWorks::SF2Patch::acmi
