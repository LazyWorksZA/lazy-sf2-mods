#include "acmi/airfields.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "acmi/acmi_format.h"

namespace LazyWorks::SF2Patch::acmi {

namespace {

bool starts_with_nocase(const std::string& s, const char* prefix) {
    const std::string l = to_lower_ascii(s);
    return l.rfind(prefix, 0) == 0;
}

bool number(const IniSection& s, const std::string& key, double& out) {
    for (const auto& kv : s.items)
        if (kv.first == key) return parse_numbers(kv.second, &out, 1);
    return false;
}

bool pair_of(const IniSection& s, const std::string& key, double out[2]) {
    for (const auto& kv : s.items)
        if (kv.first == key) return parse_numbers(kv.second, out, 2);
    return false;
}

// A flag key counts as set unless its value says no.
bool flag_set(const IniSection& s, const char* key) {
    for (const auto& kv : s.items) {
        if (kv.first != key) continue;
        const std::string v = to_lower_ascii(kv.second);
        return !(v == "0" || v == "false" || v == "no");
    }
    return false;
}

const std::string* text(const IniSection& s, const char* key) {
    for (const auto& kv : s.items)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

struct TypeInfo {
    std::string full_name;
    bool runway = false;
    bool live = false;  // UseGroundObject: the game makes it a live ground object
    bool bridge = false;
};

using TypeTable = std::map<std::string, TypeInfo>;

bool flag_true(const std::string* v) {
    if (!v) return false;
    const std::string l = to_lower_ascii(*v);
    return l == "true" || l == "1" || l == "yes";
}

TypeTable type_table(const Ini* types) {
    TypeTable out;
    if (!types) return out;
    for (const IniSection& s : types->sections) {
        if (s.name.rfind("targettype", 0) != 0) continue;
        const std::string* name = text(s, "name");
        if (!name || name->empty()) continue;
        TypeInfo info;
        if (const std::string* full = text(s, "fullname")) info.full_name = *full;
        const std::string* cat = text(s, "targettype");
        const std::string category = cat ? to_lower_ascii(*cat) : std::string();
        info.bridge = category == "bridge";
        info.runway = category.size() >= 6 && category.compare(category.size() - 6, 6, "runway") == 0;
        info.live = flag_true(text(s, "usegroundobject"));
        out.emplace(to_lower_ascii(*name), std::move(info));
    }
    return out;
}

/// <summary>A runway type: by its category when the types ini lists it, else by a type id starting "Runway".</summary>
bool is_runway(const TypeTable& table, const std::string& type) {
    const auto it = table.find(to_lower_ascii(type));
    return it != table.end() ? it->second.runway : starts_with_nocase(type, "runway");
}

struct TargetRef {
    int number;
    const std::string* type;
    std::string stem;  // "target[nnn]."
};

/// <summary>The area's Target[NNN] entries with a type, by target number.</summary>
std::vector<TargetRef> targets_of(const IniSection& area) {
    std::vector<TargetRef> out;
    for (const auto& kv : area.items) {
        const std::string& k = kv.first;
        if (k.rfind("target[", 0) != 0 || k.size() < 13 || k.compare(k.size() - 6, 6, "].type") != 0) continue;
        if (kv.second.empty()) continue;
        out.push_back({std::atoi(k.c_str() + 7), &kv.second, k.substr(0, k.size() - 4)});
    }
    std::stable_sort(out.begin(), out.end(), [](const TargetRef& a, const TargetRef& b) { return a.number < b.number; });
    return out;
}

// The Heading of the area's last runway target, in degrees (the game keeps the last one it reads).
double area_heading_deg(const IniSection& area, const TypeTable& table) {
    double h = 0;
    for (const TargetRef& t : targets_of(area)) {
        if (!is_runway(table, *t.type)) continue;
        h = 0;
        number(area, t.stem + "heading", h);
    }
    return h;
}

}  // namespace

std::vector<Airfield> airfields_from(const Ini& targets, const AirfieldLoader& loader, const Ini* types) {
    const TypeTable table = type_table(types);
    std::vector<Airfield> out;
    for (const IniSection& area : targets.sections) {
        if (area.name.rfind("targetarea", 0) != 0) continue;
        const std::string* file = text(area, "airfielddatafile");
        if (!file || file->empty() || flag_set(area, "carrierstation") || flag_set(area, "offmapairbase")) continue;
        double pos[2];
        if (!pair_of(area, "position", pos) || !std::isfinite(pos[0]) || !std::isfinite(pos[1])) continue;
        Airfield af;
        const std::string* name = text(area, "name");
        af.name = name && !name->empty() ? *name : "Airfield";
        af.x = pos[0];
        af.y = pos[1];
        // The layout is turned by the area heading whatever RotateToRunwayHeading says (SingleMission +0x41d30).
        const double a = area_heading_deg(area, table) / kRadToDeg;
        af.heading = a;
        const std::vector<uint8_t> bytes = loader ? loader(*file) : std::vector<uint8_t>{};
        if (!bytes.empty()) {
            const Ini ini = parse_ini(bytes.data(), bytes.size());
            for (int i = 1; i <= kMaxRunways; ++i) {
                char sec[16];
                std::snprintf(sec, sizeof(sec), "Runway%03d", i);
                const IniSection* r = ini.find(sec);
                if (!r) continue;
                double heading = 0, off[2] = {0, 0}, length = 0, width = 0;
                number(*r, "heading", heading);
                pair_of(*r, "offset", off);
                if (!number(*r, "length", length) || !number(*r, "width", width)) continue;
                Runway rw;
                rw.x = af.x + off[0] * std::cos(a) + off[1] * std::sin(a);
                rw.y = af.y - off[0] * std::sin(a) + off[1] * std::cos(a);
                rw.heading = a + heading / kRadToDeg;
                rw.length = length;
                rw.width = width;
                af.runways.push_back(rw);
            }
        }
        if (!af.runways.empty()) af.heading = af.runways[0].heading;
        out.push_back(std::move(af));
    }
    return out;
}

PlacedStructures structures_from(const Ini& targets, const Ini& types) {
    const TypeTable table = type_table(&types);
    PlacedStructures out;
    for (const IniSection& area : targets.sections) {
        if (area.name.rfind("targetarea", 0) != 0) continue;
        if (flag_set(area, "carrierstation") || flag_set(area, "offmapairbase")) continue;
        double pos[2];
        if (!pair_of(area, "position", pos) || !std::isfinite(pos[0]) || !std::isfinite(pos[1])) continue;
        const std::string* file = text(area, "airfielddatafile");
        const bool airfield = file && !file->empty();
        const bool rotate = airfield && (!text(area, "rotatetorunwayheading") || flag_set(area, "rotatetorunwayheading"));
        TargetArea ta;
        ta.x = pos[0];
        ta.y = pos[1];
        double radius = 0;
        if (number(area, "radius", radius) && std::isfinite(radius) && radius > 0) ta.radius = radius;
        const uint32_t index = static_cast<uint32_t>(out.areas.size());
        double a = 0;
        bool turned = false;  // a runway target has been read: later targets are turned by its heading
        for (const TargetRef& t : targets_of(area)) {
            const auto it = table.find(to_lower_ascii(*t.type));
            double off[2] = {0, 0}, heading = 0;
            pair_of(area, t.stem + "offset", off);
            number(area, t.stem + "heading", heading);
            if (!std::isfinite(off[0]) || !std::isfinite(off[1]) || !std::isfinite(heading)) continue;
            ta.radius = std::max(ta.radius, std::hypot(off[0], off[1]));
            if (is_runway(table, *t.type)) {
                a = heading / kRadToDeg;
                turned = rotate;
                continue;
            }
            if (it != table.end() && it->second.live) continue;
            const double r = turned ? a : 0.0;
            const double ca = std::cos(r), sa = std::sin(r);
            Structure s;
            s.name = it != table.end() && !it->second.full_name.empty() ? it->second.full_name : *t.type;
            s.bridge = it != table.end() && it->second.bridge;
            s.x = pos[0] + off[0] * ca + off[1] * sa;
            s.y = pos[1] - off[0] * sa + off[1] * ca;
            s.heading = r + heading / kRadToDeg;
            s.area = index;
            out.items.push_back(std::move(s));
        }
        out.areas.push_back(ta);
    }
    return out;
}

size_t pick_areas(const TargetArea* areas, size_t n, const AreaPick& pick, uint8_t* out) {
    size_t picked = 0;
    const double near2 = kAreaMatchMetres * kAreaMatchMetres;
    for (size_t i = 0; i < n; ++i) {
        const TargetArea& a = areas[i];
        bool on = false;
        if (pick.use_active) {
            for (size_t k = 0; k < pick.n_active && !on; ++k) {
                const double dx = pick.active[k][0] - a.x, dy = pick.active[k][1] - a.y;
                on = dx * dx + dy * dy <= near2;
            }
        } else {
            if (pick.have_player) {
                const double reach = a.radius + pick.range;
                const double dx = pick.player[0] - a.x, dy = pick.player[1] - a.y;
                on = dx * dx + dy * dy <= reach * reach;
            }
            const double r2 = a.radius * a.radius;
            for (size_t k = 0; k < pick.n_ground && !on; ++k) {
                const double dx = pick.ground[k][0] - a.x, dy = pick.ground[k][1] - a.y;
                on = dx * dx + dy * dy <= r2;
            }
        }
        out[i] = on ? 1 : 0;
        picked += on ? 1 : 0;
    }
    return picked;
}

size_t find_near(const double (*pts)[2], size_t n, double x, double y, double tol, const uint8_t* taken) {
    size_t best = n;
    double best2 = tol * tol;
    for (size_t i = 0; i < n; ++i) {
        if (taken && taken[i]) continue;
        const double dx = pts[i][0] - x, dy = pts[i][1] - y, d2 = dx * dx + dy * dy;
        if (d2 <= best2) {
            best = i;
            best2 = d2;
        }
    }
    return best;
}

size_t utf8_prefix(const std::string& s, size_t max) {
    if (s.size() <= max) return s.size();
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;  // s[n] continues a character: cut before it
    return n;
}

std::string types_file_name(const Ini& theatre, const std::string& targets_file) {
    if (const std::string* v = theatre.get("terrain", "targettypefile"); v && !v->empty()) return *v;
    const std::string l = to_lower_ascii(targets_file);
    const size_t at = l.rfind("_targets");
    if (at == std::string::npos) return {};
    return targets_file.substr(0, at) + "_TYPES" + targets_file.substr(at + 8);
}

double height_at(const uint16_t* samples, int w, int h, double spacing, double x, double y) {
    if (!samples || w < 2 || h < 2 || !(spacing > 0) || !std::isfinite(x) || !std::isfinite(y)) return 0;
    const double fx = x / spacing, fy = y / spacing;
    if (fx < 0 || fy < 0 || fx > w - 1 || fy > h - 1) return 0;
    const int i = std::min(static_cast<int>(fx), w - 2), j = std::min(static_cast<int>(fy), h - 2);
    const double u = fx - i, v = fy - j;
    const auto s = [&](int xx, int yy) { return static_cast<double>(samples[static_cast<size_t>(yy) * static_cast<size_t>(w) + static_cast<size_t>(xx)]); };
    return (s(i, j) * (1 - u) + s(i + 1, j) * u) * (1 - v) + (s(i, j + 1) * (1 - u) + s(i + 1, j + 1) * u) * v;
}

}  // namespace LazyWorks::SF2Patch::acmi
