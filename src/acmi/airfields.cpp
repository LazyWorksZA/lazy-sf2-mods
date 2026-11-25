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

// The Heading of the area's first runway-category target (Target[NNN].Type starting with "Runway"), in degrees.
double area_heading_deg(const IniSection& area) {
    for (const auto& kv : area.items) {
        const std::string& k = kv.first;
        if (k.rfind("target[", 0) != 0 || k.size() < 13 || k.compare(k.size() - 6, 6, "].type") != 0) continue;
        if (!starts_with_nocase(kv.second, "runway")) continue;
        double h = 0;
        if (number(area, k.substr(0, k.size() - 4) + "heading", h)) return h;
        return 0;
    }
    return 0;
}

}  // namespace

std::vector<Airfield> airfields_from(const Ini& targets, const AirfieldLoader& loader) {
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
        const double a = area_heading_deg(area) / kRadToDeg;
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

namespace {

struct TypeInfo {
    std::string full_name;
    bool skip = false;  // a runway, or a type the game makes a live ground object
    bool bridge = false;
};

bool flag_true(const std::string* v) {
    if (!v) return false;
    const std::string l = to_lower_ascii(*v);
    return l == "true" || l == "1" || l == "yes";
}

std::map<std::string, TypeInfo> type_table(const Ini& types) {
    std::map<std::string, TypeInfo> out;
    for (const IniSection& s : types.sections) {
        if (s.name.rfind("targettype", 0) != 0) continue;
        const std::string* name = text(s, "name");
        if (!name || name->empty()) continue;
        TypeInfo info;
        if (const std::string* full = text(s, "fullname")) info.full_name = *full;
        const std::string* cat = text(s, "targettype");
        const std::string category = cat ? to_lower_ascii(*cat) : std::string();
        info.bridge = category == "bridge";
        info.skip = flag_true(text(s, "usegroundobject")) ||
                    (category.size() >= 6 && category.compare(category.size() - 6, 6, "runway") == 0);
        out.emplace(to_lower_ascii(*name), std::move(info));
    }
    return out;
}

}  // namespace

std::vector<Structure> structures_from(const Ini& targets, const Ini& types) {
    const std::map<std::string, TypeInfo> table = type_table(types);
    std::vector<Structure> out;
    for (const IniSection& area : targets.sections) {
        if (area.name.rfind("targetarea", 0) != 0) continue;
        if (flag_set(area, "carrierstation") || flag_set(area, "offmapairbase")) continue;
        double pos[2];
        if (!pair_of(area, "position", pos) || !std::isfinite(pos[0]) || !std::isfinite(pos[1])) continue;
        const bool rotate = !text(area, "rotatetorunwayheading") || flag_set(area, "rotatetorunwayheading");
        const double a = rotate ? area_heading_deg(area) / kRadToDeg : 0.0;
        const double ca = std::cos(a), sa = std::sin(a);
        for (const auto& kv : area.items) {
            const std::string& k = kv.first;
            if (k.rfind("target[", 0) != 0 || k.size() < 13 || k.compare(k.size() - 6, 6, "].type") != 0) continue;
            if (kv.second.empty()) continue;
            const auto it = table.find(to_lower_ascii(kv.second));
            if (it == table.end() ? starts_with_nocase(kv.second, "runway") : it->second.skip) continue;
            const std::string stem = k.substr(0, k.size() - 4);  // "target[nnn]."
            double off[2] = {0, 0}, heading = 0;
            pair_of(area, stem + "offset", off);
            number(area, stem + "heading", heading);
            if (!std::isfinite(off[0]) || !std::isfinite(off[1]) || !std::isfinite(heading)) continue;
            Structure s;
            s.name = it != table.end() && !it->second.full_name.empty() ? it->second.full_name : kv.second;
            s.bridge = it != table.end() && it->second.bridge;
            s.x = pos[0] + off[0] * ca + off[1] * sa;
            s.y = pos[1] - off[0] * sa + off[1] * ca;
            s.heading = a + heading / kRadToDeg;
            out.push_back(std::move(s));
        }
    }
    return out;
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
