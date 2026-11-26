#pragma once
// Pure parts of the shared-world tooling (world_sync.cpp): the world manifest lines and the mission sidecar. Free of
// Windows headers so the tests can use them. See lazy-sf2-recon: network-recon §10.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <vector>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>One manifest line: an aircraft or ground-group member, an empty slot, or a placed record.</summary>
struct ManifestEntry {
    enum Kind : char { Aircraft = 'A', Ground = 'G', Placed = 'P' };
    enum State : uint8_t {
        Object,      // a live object: type, side and pose are filled in
        Absent,      // a slot whose spawn draw failed (no formation): one line, member -1
        Unresolved,  // a member id that names no live object
        Model,       // a placed record drawn as a plain model (pose = the record's position)
        None,        // a placed record that is neither
    };
    char kind = Aircraft;
    int a = 0;   // slot index, or area index
    int b = 0;   // member index, or the placed record's index (+0x54); -1 for a whole-slot line
    State state = Object;
    std::string type;  // ASCII type name; may be empty
    char side = '?';   // F, E, N or ?
    double pos[3] = {};
    float heading = 0;  // radians
    bool has_pose = false;
    bool has_id = false;
    uint32_t id = 0;  // the game's object id: noted, but not part of the key (it depends on what the groups held)
};

/// <summary>Sorts by kind (A, G, P), then a, then b. Stable, so equal keys keep their order.</summary>
inline void sort_manifest(std::vector<ManifestEntry>& v) {
    auto rank = [](char k) { return k == ManifestEntry::Aircraft ? 0 : k == ManifestEntry::Ground ? 1 : 2; };
    std::stable_sort(v.begin(), v.end(), [&](const ManifestEntry& x, const ManifestEntry& y) {
        return std::make_tuple(rank(x.kind), x.a, x.b) < std::make_tuple(rank(y.kind), y.a, y.b);
    });
}

/// <summary>Heading in tenths of a degree, 0..3599 (radians in; rounded, then wrapped).</summary>
inline int heading_tenths(float rad) {
    if (!std::isfinite(rad)) return 0;
    long t = std::lround(static_cast<double>(rad) * (1800.0 / 3.14159265358979323846));
    t %= 3600;
    if (t < 0) t += 3600;
    return static_cast<int>(t);
}

/// <summary>A coordinate rounded to the metre; not finite gives 0 and sets *bad.</summary>
inline long round_metre(double v, bool* bad) {
    if (!std::isfinite(v) || std::fabs(v) > 1e9) {
        *bad = true;
        return 0;
    }
    return std::lround(v);
}

/// <summary>
/// One manifest line, without a newline. The key comes first ("A 003.01"); the raw id is the last field
/// (" id=0x...") so a diff can strip it.
/// </summary>
inline std::string format_manifest_line(const ManifestEntry& e) {
    char buf[256];
    int n = 0;
    if (e.kind == ManifestEntry::Placed) {
        n = std::snprintf(buf, sizeof(buf), "P %04d.%04d", e.a, e.b);
    } else if (e.b < 0) {
        n = std::snprintf(buf, sizeof(buf), "%c %03d.--", e.kind, e.a);
    } else {
        n = std::snprintf(buf, sizeof(buf), "%c %03d.%02d", e.kind, e.a, e.b);
    }
    std::string s(buf, n > 0 ? static_cast<size_t>(n) : 0);
    static const char* const kState[] = {"", "absent", "unresolved", "model", "none"};
    if (e.state != ManifestEntry::Object) {
        s += ' ';
        s += kState[e.state];
    } else {
        s += " type=";
        s += e.type.empty() ? "?" : e.type;
        s += " side=";
        s += e.side;
    }
    if (e.has_pose) {
        bool bad = false;
        const long x = round_metre(e.pos[0], &bad), y = round_metre(e.pos[1], &bad), z = round_metre(e.pos[2], &bad);
        if (bad) {
            s += " pos=bad";
        } else {
            n = std::snprintf(buf, sizeof(buf), " pos=%ld,%ld,%ld", x, y, z);
            s.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
        }
        if (e.state == ManifestEntry::Object) {
            const int h = heading_tenths(e.heading);
            n = std::snprintf(buf, sizeof(buf), " hdg=%d.%d", h / 10, h % 10);
            s.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
        }
    }
    if (e.has_id) {
        n = std::snprintf(buf, sizeof(buf), " id=0x%08x", e.id);
        s.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
    }
    return s;
}

/// <summary>The counts line ("C aircraft=12 ..."): per kind, the lines in each state.</summary>
inline std::string format_manifest_counts(const std::vector<ManifestEntry>& v) {
    int ao = 0, aa = 0, au = 0, go = 0, ga = 0, gu = 0, po = 0, pm = 0, pn = 0, pu = 0;
    for (const ManifestEntry& e : v) {
        const int obj = e.state == ManifestEntry::Object, gone = e.state == ManifestEntry::Absent,
                  unr = e.state == ManifestEntry::Unresolved;
        if (e.kind == ManifestEntry::Aircraft) {
            ao += obj, aa += gone, au += unr;
        } else if (e.kind == ManifestEntry::Ground) {
            go += obj, ga += gone, gu += unr;
        } else {
            po += obj, pm += e.state == ManifestEntry::Model, pn += e.state == ManifestEntry::None, pu += unr;
        }
    }
    char buf[256];
    const int n = std::snprintf(buf, sizeof(buf),
                                "C aircraft=%d aircraft_absent_slots=%d aircraft_unresolved=%d ground=%d "
                                "ground_absent_slots=%d ground_unresolved=%d placed_objects=%d placed_models=%d "
                                "placed_none=%d placed_unresolved=%d",
                                ao, aa, au, go, ga, gu, po, pm, pn, pu);
    return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

/// <summary>
/// The whole manifest: header lines as given (run-specific ones should start with '#'), the sorted entries, then the
/// counts line. Sorts v.
/// </summary>
inline std::string format_manifest(const std::vector<std::string>& header, std::vector<ManifestEntry>& v) {
    sort_manifest(v);
    std::string out;
    for (const std::string& h : header) out += h + "\n";
    for (const ManifestEntry& e : v) out += format_manifest_line(e) + "\n";
    out += format_manifest_counts(v) + "\n";
    return out;
}

/// <summary>
/// What a saved world needs besides the .MSN file: the seed, and the options read at mission start that the file does
/// not carry. Text, one key=value per line.
/// </summary>
struct WorldSidecar {
    static constexpr int kVersion = 1;
    std::string msn;            // the .MSN file name (no folder)
    bool has_seed = false;
    uint32_t seed = 0;          // RND.Seed value, before Load and at the start call
    int density = -1;           // [GraphicsOptions] GroundObjectDensity 0..3; -1 = not known yet
    int mission_start = -1;     // [GameplayOptions] MissionStart 0..3; -1 = not known yet
    std::string theatre;        // MissionMap (ASCII)
    std::string aircraft;       // the player's AircraftType (ASCII)
    int mission_type = -1;
    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;
    int weather = -1;           // WeatherType index (the file stores it + 1)
};

inline std::string format_sidecar(const WorldSidecar& s) {
    char buf[512];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "# Lazy SF2 Patch world sidecar (developer tool)\nversion=%d\nmsn=%s\nseed=%lu\ndensity=%d\nmission_start=%d\n"
        "theatre=%s\naircraft=%s\nmission_type=%d\ndate=%04d-%02d-%02d\ntime=%02d:%02d:%02d\nweather=%d\n",
        WorldSidecar::kVersion, s.msn.c_str(), static_cast<unsigned long>(s.seed), s.density, s.mission_start,
        s.theatre.c_str(), s.aircraft.c_str(), s.mission_type, s.year, s.month, s.day, s.hour, s.minute, s.second,
        s.weather);
    return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

namespace detail {
inline bool parse_long(const std::string& t, long lo, long hi, long* out) {
    if (t.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (*end || v < lo || v > hi) return false;
    *out = v;
    return true;
}
inline bool parse_ulong(const std::string& t, unsigned long* out) {
    if (t.empty() || t[0] == '-' || t[0] == '+') return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 10);
    if (*end || v > 0xffffffffull) return false;
    *out = static_cast<unsigned long>(v);
    return true;
}
// "a<sep>b<sep>c" with three numbers.
inline bool parse_triple(const std::string& t, char sep, int* a, int* b, int* c) {
    const size_t p = t.find(sep), q = p == std::string::npos ? p : t.find(sep, p + 1);
    if (q == std::string::npos) return false;
    long x = 0, y = 0, z = 0;
    if (!parse_long(t.substr(0, p), 0, 99999, &x) || !parse_long(t.substr(p + 1, q - p - 1), 0, 99999, &y) ||
        !parse_long(t.substr(q + 1), 0, 99999, &z))
        return false;
    *a = static_cast<int>(x), *b = static_cast<int>(y), *c = static_cast<int>(z);
    return true;
}
}  // namespace detail

/// <summary>
/// Parses a sidecar. Blank lines, '#' comments, CR and unknown keys are ignored. Fails (with *error) on a missing or
/// bad seed, a version other than 1, or a malformed known value.
/// </summary>
inline bool parse_sidecar(const std::string& text, WorldSidecar* out, std::string* error) {
    WorldSidecar s;
    bool version_seen = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            *error = "line without '=': " + line;
            return false;
        }
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        long n = 0;
        unsigned long u = 0;
        bool ok = true;
        if (k == "version") {
            ok = detail::parse_long(v, 0, 1000, &n) && n == WorldSidecar::kVersion;
            version_seen = ok;
        } else if (k == "msn") {
            s.msn = v;
        } else if (k == "seed") {
            ok = detail::parse_ulong(v, &u);
            s.seed = static_cast<uint32_t>(u);
            s.has_seed = ok;
        } else if (k == "density") {
            ok = detail::parse_long(v, -1, 3, &n);
            s.density = static_cast<int>(n);
        } else if (k == "mission_start") {
            ok = detail::parse_long(v, -1, 3, &n);
            s.mission_start = static_cast<int>(n);
        } else if (k == "theatre") {
            s.theatre = v;
        } else if (k == "aircraft") {
            s.aircraft = v;
        } else if (k == "mission_type") {
            ok = detail::parse_long(v, -1, 1000, &n);
            s.mission_type = static_cast<int>(n);
        } else if (k == "date") {
            ok = detail::parse_triple(v, '-', &s.year, &s.month, &s.day);
        } else if (k == "time") {
            ok = detail::parse_triple(v, ':', &s.hour, &s.minute, &s.second);
        } else if (k == "weather") {
            ok = detail::parse_long(v, -1, 1000, &n);
            s.weather = static_cast<int>(n);
        }
        if (!ok) {
            *error = "bad value: " + line;
            return false;
        }
        if (eol == text.size()) break;
    }
    if (!version_seen) {
        *error = "no version=1 line";
        return false;
    }
    if (!s.has_seed) {
        *error = "no seed";
        return false;
    }
    *out = s;
    return true;
}

/// <summary>
/// Parses one entry line of a manifest (format_manifest_line's output) back; false for header, counts and bad lines.
/// The raw id is read when present.
/// </summary>
inline bool parse_manifest_line(const std::string& line, ManifestEntry* out) {
    ManifestEntry e;
    if (line.size() < 8 || line[1] != ' ') return false;
    if (line[0] != 'A' && line[0] != 'G' && line[0] != 'P') return false;
    e.kind = line[0];
    const size_t dot = line.find('.', 2), sp = line.find(' ', 2);
    if (dot == std::string::npos || (sp != std::string::npos && dot > sp)) return false;
    const std::string sa = line.substr(2, dot - 2), sb = line.substr(dot + 1, (sp == std::string::npos ? line.size() : sp) - dot - 1);
    long a = 0, b = 0;
    if (!detail::parse_long(sa, 0, 99999, &a)) return false;
    if (sb == "--") b = -1;
    else if (!detail::parse_long(sb, 0, 99999, &b)) return false;
    e.a = static_cast<int>(a), e.b = static_cast<int>(b);
    size_t pos = sp;
    bool typed = false;
    while (pos != std::string::npos && pos < line.size()) {
        const size_t next = line.find(' ', pos + 1);
        const std::string f = line.substr(pos + 1, (next == std::string::npos ? line.size() : next) - pos - 1);
        pos = next;
        if (f == "absent") e.state = ManifestEntry::Absent;
        else if (f == "unresolved") e.state = ManifestEntry::Unresolved;
        else if (f == "model") e.state = ManifestEntry::Model;
        else if (f == "none") e.state = ManifestEntry::None;
        else if (f.rfind("type=", 0) == 0) e.type = f.substr(5), typed = true;
        else if (f.rfind("side=", 0) == 0 && f.size() == 6) e.side = f[5];
        else if (f.rfind("pos=", 0) == 0) {
            double v[3];
            if (sscanf_s(f.c_str() + 4, "%lf,%lf,%lf", &v[0], &v[1], &v[2]) != 3) return false;
            e.pos[0] = v[0], e.pos[1] = v[1], e.pos[2] = v[2];
            e.has_pose = true;
        } else if (f.rfind("hdg=", 0) == 0) {
            e.heading = static_cast<float>(std::atof(f.c_str() + 4) * 3.14159265358979323846 / 180.0);
        } else if (f.rfind("id=0x", 0) == 0) {
            e.id = static_cast<uint32_t>(std::strtoul(f.c_str() + 5, nullptr, 16));
            e.has_id = true;
        }
    }
    if (typed) e.state = ManifestEntry::Object;
    *out = e;
    return true;
}

/// <summary>NNN of a "world_NNN.<anything>" file name (case-insensitive prefix), or -1.</summary>
inline int world_file_number(const wchar_t* name) {
    static const wchar_t kPrefix[] = L"world_";
    for (int i = 0; kPrefix[i]; ++i) {
        wchar_t c = name[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c != kPrefix[i]) return -1;
    }
    const wchar_t* p = name + 6;
    long v = 0;
    int digits = 0;
    while (*p >= L'0' && *p <= L'9' && digits < 6) v = v * 10 + (*p++ - L'0'), ++digits;
    if (digits == 0 || *p != L'.') return -1;
    return static_cast<int>(v);
}

/// <summary>path with a trailing ".msn" (any case) replaced by suffix; without one, suffix is appended.</summary>
inline std::wstring world_companion_path(const std::wstring& path, const wchar_t* suffix) {
    std::wstring base = path;
    if (base.size() >= 4) {
        std::wstring ext = base.substr(base.size() - 4);
        for (wchar_t& c : ext)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (ext == L".msn") base.resize(base.size() - 4);
    }
    return base + suffix;
}

}  // namespace LazyWorks::SF2Patch::hooks
