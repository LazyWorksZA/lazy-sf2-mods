#include "acmi/airfields.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "acmi/acmi_format.h"

namespace lazysf2::acmi {

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

}  // namespace lazysf2::acmi
