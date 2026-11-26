#include "acmi/object_types.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

#include "acmi/tacview_core.h"

namespace LazyWorks::SF2Patch::acmi {

namespace {

std::string lower(std::string_view s) {
    std::string n(s);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return n;
}

bool has_any(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

/// "sa-2", "sa-13": a surface-to-air missile designation.
bool has_sa_designation(const std::string& s) {
    for (size_t p = s.find("sa-"); p != std::string::npos; p = s.find("sa-", p + 1)) {
        const bool start = p == 0 || !std::isalpha(static_cast<unsigned char>(s[p - 1]));
        if (start && p + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[p + 3]))) return true;
    }
    return false;
}

bool is_anti_aircraft(const std::string& n) {
    return has_any(n, {"aaa", "flak", "zsu", "anti-air", "antiair", "aa gun", "sam ", " sam", "missile site"}) ||
           n == "sam" || has_sa_designation(n);
}

bool is_storage(const std::string& n) {
    return has_any(n, {"fuel tank", "storage tank", "oil tank", "tank farm", "fuel depot", "fuel dump", "fuel storage",
                       "oil storage", "silo"});
}

const char* vehicle_tags(const std::string& n) {
    if (is_anti_aircraft(n)) return "Ground+AntiAircraft";
    if (has_any(n, {"truck", "lorry", "convoy", "supply", "ammo", "tanker", "bus"})) return "Ground+Medium+Vehicle";
    if (has_any(n, {"jeep", "car", "pickup", "van", "staff", "scout", "buggy", "motorcycle", "utility"}))
        return "Ground+Light+Vehicle";
    if (has_any(n, {"infantry", "soldier", "troop", "squad", "platoon"})) return "Ground+Light+Human+Infantry";
    if (has_any(n, {"apc", "armored", "armoured", "personnel carrier", "ifv", "bmp", "btr"}))
        return "Ground+Heavy+Armor+Vehicle";
    if (has_any(n, {"tank", "mbt"})) return "Ground+Heavy+Armor+Vehicle+Tank";
    if (has_any(n, {"howitzer", "artillery", "mortar", "mlrs", "rocket launcher", "self-propelled", "gun"}))
        return "Ground+Heavy+Armor+Vehicle";
    return "Ground+Vehicle";
}

const char* ship_tags(const std::string& n) {
    if (has_any(n, {"carrier", "cvn", "cva"})) return "Sea+Watercraft+AircraftCarrier";
    if (has_any(n, {"submarine"})) return "Sea+Watercraft+Submarine";
    if (has_any(n, {"destroyer", "cruiser", "frigate", "corvette", "battleship", "gunboat", "patrol", "missile boat",
                    "warship", "minesweeper", "landing ship"}))
        return "Sea+Watercraft+Warship";
    return "Sea+Watercraft";
}

const char* static_tags(const std::string& n) {
    if (is_storage(n)) return "Ground+Static+Container";
    if (is_anti_aircraft(n) || n.find("radar") != std::string::npos) return "Ground+AntiAircraft";
    return "Ground+Static+Building";
}

}  // namespace

bool is_storage_category(std::string_view category) {
    const std::string c = lower(category);
    return has_any(c, {"fuel", "storage", "oil", "pol"});
}

const char* ground_type_tags(uint8_t cls, std::string_view name) {
    const std::string n = lower(name);
    switch (cls) {
        case TacviewRec::kShip: return ship_tags(n);
        case TacviewRec::kGroundVehicle: return vehicle_tags(n);
        case TacviewRec::kBridge: return "Ground+Static+Building";  // the SDK defines no Bridge tag
        case TacviewRec::kStorage: return "Ground+Static+Container";
        default: return static_tags(n);
    }
}

}  // namespace LazyWorks::SF2Patch::acmi
