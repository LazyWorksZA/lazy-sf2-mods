#include "acmi/acmi_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace LazyWorks::SF2Patch::acmi {

Mapping make_mapping(double ref_lat_deg) {
    return {1.0 / (kMetresPerDegree * std::cos(ref_lat_deg / kRadToDeg)), 1.0 / kMetresPerDegree};
}

bool geo_is_set(double lon, double lat) {
    if (!std::isfinite(lon) || !std::isfinite(lat) || lon < -180.0 || lon > 180.0 || lat < -85.0 || lat > 85.0) return false;
    // The engine reads floats and widens them, so the default arrives as (double)-96.8f.
    return !(static_cast<float>(lon) == -96.8f && static_cast<float>(lat) == 30.2f);
}

Placement place_theatre(bool has_geo, double lon, double lat, double extent_x_m, double extent_y_m) {
    if (!has_geo) return {kDefaultRefLon, kDefaultRefLat, kDefaultRefLat};
    const Mapping m = make_mapping(lat);
    return {std::clamp(lon - extent_x_m / 2 * m.lon_scale, -180.0, 180.0),
            std::clamp(lat - extent_y_m / 2 * m.lat_scale, -89.0, 89.0), lat};
}

std::string num(double v, int decimals) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", decimals, v);
    std::string r = b;
    if (r.find('.') != std::string::npos) {
        while (!r.empty() && r.back() == '0') r.pop_back();
        if (!r.empty() && r.back() == '.') r.pop_back();
    }
    if (r == "-0" || r.empty()) r = "0";
    return r;
}

double norm_deg(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0) d += 360.0;
    if (d >= 359.995) return 0.0;  // prints as 360.00 with two decimals: the same direction as 0
    return d + 0.0;                // -0.0 becomes +0.0
}

size_t utf8_seq_len(const unsigned char* s, size_t avail) {
    if (avail < 2 || s[0] < 0xc2 || s[0] > 0xf4) return 0;
    const size_t n = s[0] < 0xe0 ? 2 : s[0] < 0xf0 ? 3 : 4;
    if (avail < n) return 0;
    for (size_t i = 1; i < n; ++i)
        if ((s[i] & 0xc0) != 0x80) return 0;
    if (s[0] == 0xe0 && s[1] < 0xa0) return 0;   // overlong
    if (s[0] == 0xed && s[1] >= 0xa0) return 0;  // surrogate
    if (s[0] == 0xf0 && s[1] < 0x90) return 0;   // overlong
    if (s[0] == 0xf4 && s[1] >= 0x90) return 0;  // above U+10FFFF
    return n;
}

void append_utf8_cp(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp >= 0xd800 && cp < 0xe000) {
        out += '?';
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp <= 0x10ffff) {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += '?';
    }
}

void append_escaped(std::string& out, const char* s) {
    if (!s) return;
    const size_t len = std::strlen(s);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const size_t n = utf8_seq_len(reinterpret_cast<const unsigned char*>(s) + i, len - i);
            if (n) {
                out.append(s + i, n);
                i += n - 1;
            } else {
                out += '?';
            }
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            out += '?';
            continue;
        }
        if (c == ',') out += '\\';
        out += static_cast<char>(c);
    }
}

void append_header(std::string& out, const char* reference_time, double ref_lon, double ref_lat, const char* title,
                   const char* theater) {
    out += "FileType=text/acmi/tacview\nFileVersion=2.2\n";
    // ReferenceLongitude/Latitude are added to each object's Longitude/Latitude (spec, "Global properties"),
    // so the T= values are offsets from the reference point.
    out += "0,ReferenceTime=";
    out += reference_time;
    out += ",ReferenceLongitude=" + num(ref_lon, 9) + ",ReferenceLatitude=" + num(ref_lat, 9) + ",Title=";
    append_escaped(out, title);
    // Tacview's "Auto" terrain layer set picks layers from DataSource; an unknown source selects Real World, where
    // the Lazy SF2 Patch terrain lives. Without it Tacview keeps the last set used (for example BMS) and hides our terrain.
    out += ",DataSource=Strike Fighters 2,DataRecorder=Lazy SF2 Patch";
    if (theater && *theater) {
        out += ",Theater=";
        append_escaped(out, theater);
        // The add-on reads this one first: Tacview may not expose a global property named Theater to Lua.
        out += ",LazySF2PatchTheater=";
        append_escaped(out, theater);
    }
    out += '\n';
}

void append_frame(std::string& out, long long ms) {
    if (ms < 0) ms = 0;
    char b[48];
    std::snprintf(b, sizeof(b), "#%lld.%03lld\n", ms / 1000, ms % 1000);
    out += b;
}

void append_removal(std::string& out, uint32_t id) {
    char b[24];
    std::snprintf(b, sizeof(b), "-%x\n", id);
    out += b;
}

void append_lock(std::string& out, uint32_t id, uint32_t target) {
    char b[64];
    if (target)
        std::snprintf(b, sizeof(b), "%x,LockedTarget=%x,LockedTargetMode=1\n", id, target);
    else
        std::snprintf(b, sizeof(b), "%x,LockedTargetMode=0,LockedTarget=\n", id);
    out += b;
}

void append_event_destroyed(std::string& out, uint32_t id) {
    char b[48];
    std::snprintf(b, sizeof(b), "0,Event=Destroyed|%x|\n", id);
    out += b;
}

void format_transform(TransformText& out, const double pos[3], const float ang[3], const Mapping& m) {
    const double heading = norm_deg(static_cast<double>(ang[1]) * kRadToDeg);
    out.f[0] = num(pos[0] * m.lon_scale, 9);
    out.f[1] = num(pos[1] * m.lat_scale, 9);
    out.f[2] = num(pos[2], 2);
    out.f[3] = num(static_cast<double>(ang[2]) * kRadToDeg, 2);
    out.f[4] = num(static_cast<double>(ang[0]) * kRadToDeg, 2);
    out.f[5] = num(heading, 2);
    out.f[6] = num(pos[0], 2);
    out.f[7] = num(pos[1], 2);
    out.f[8] = num(heading, 2);
}

void append_transform(std::string& out, uint32_t id, const double pos[3], const float ang[3], const Mapping& m) {
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%x", id);
    TransformText t;
    format_transform(t, pos, ang, m);
    out += idbuf;
    out += ",T=";
    for (int i = 0; i < 9; ++i) {
        if (i) out += '|';
        out += t.f[i];
    }
}

bool append_transform_delta(std::string& out, uint32_t id, const TransformText& cur, TransformText& last, bool first) {
    bool full = first;
    for (const std::string& s : last.f) full = full || s.empty();
    bool changed = full;
    if (!full) {
        for (int i = 0; i < 9; ++i) changed = changed || cur.f[i] != last.f[i];
    }
    if (!changed) return false;
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%x", id);
    out += idbuf;
    out += ",T=";
    for (int i = 0; i < 9; ++i) {
        if (i) out += '|';
        if (full || cur.f[i] != last.f[i]) out += cur.f[i];
    }
    last = cur;
    return true;
}

void append_position(std::string& out, uint32_t id, const double pos[3], const Mapping& m) {
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%x", id);
    out += idbuf;
    out += ",T=" + num(pos[0] * m.lon_scale, 9) + "|" + num(pos[1] * m.lat_scale, 9) + "|" + num(pos[2], 2);
}

void append_props(std::string& out, const ObjectProps& p) {
    if (p.name && *p.name) {
        out += ",Name=";
        append_escaped(out, p.name);
    }
    if (p.pilot && *p.pilot) {
        out += ",Pilot=";
        append_escaped(out, p.pilot);
    }
    if (p.type && *p.type) {
        out += ",Type=";
        append_escaped(out, p.type);
    }
    if (p.coalition && *p.coalition) {
        out += ",Coalition=";
        append_escaped(out, p.coalition);
    }
    if (p.color && *p.color) {
        out += ",Color=";
        append_escaped(out, p.color);
    }
    if (p.parent && *p.parent) {
        out += ",Parent=";
        append_escaped(out, p.parent);
    }
}

}  // namespace LazyWorks::SF2Patch::acmi
