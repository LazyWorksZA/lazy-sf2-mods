// ACMI 2.2 text formatting shared by the offline converter (tools/acmi) and the in-game Tacview recorder
// (src/acmi/tacview_core.*). Pure: no Windows or game dependencies. Spec: Tacview "ACMI telemetry file format".
#pragma once

#include <cstdint>
#include <string>

namespace lazysf2::acmi {

constexpr double kMetresPerDegree = 111320.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

// The local flat mapping: metres east/north of the reference point to degrees of longitude/latitude offset.
struct Mapping {
    double lon_scale = 0;
    double lat_scale = 0;
};
Mapping make_mapping(double ref_lat_deg);

// Fixed decimals with trailing zeros trimmed (smaller files); "-0" becomes "0".
std::string num(double v, int decimals);
// Degrees into [0, 360); a value that would print as 360.00 becomes 0, and -0.0 becomes +0.0.
double norm_deg(double d);
// Appends s with ',' escaped by a backslash (spec, "Escaping"). Control characters and non-ASCII bytes become '?',
// so a value can never end a line.
void append_escaped(std::string& out, const char* s);

// "FileType/FileVersion" lines and the global object (id 0). reference_time is e.g. 2026-11-24T20:15:00Z.
void append_header(std::string& out, const char* reference_time, double ref_lon, double ref_lat, const char* title);
// "#seconds.mmm\n" for a time in whole milliseconds since the start (never negative).
void append_frame(std::string& out, long long ms);
// "-id\n"
void append_removal(std::string& out, uint32_t id);
// "id,T=Lon|Lat|Alt|Roll|Pitch|Yaw|U|V|Heading" without the newline (the caller may append properties first).
// pos is (east, north, up) in metres, ang is (pitch, heading, roll) in radians as the game keeps them:
// pitch nose-up positive, heading clockwise from north, roll right-wing-down positive. Yaw and heading are equal,
// because the flat world is aligned with north.
void append_transform(std::string& out, uint32_t id, const double pos[3], const float ang[3], const Mapping& m);

// Properties written after the first transform of an object. A null or empty member is left out.
struct ObjectProps {
    const char* name = nullptr;
    const char* pilot = nullptr;
    const char* type = nullptr;       // e.g. "Air+FixedWing"
    const char* coalition = nullptr;  // e.g. "Allies"
    const char* color = nullptr;      // e.g. "Blue"
    const char* parent = nullptr;     // the parent object's id in hex, e.g. "10003" (weapons: the shooter)
};
void append_props(std::string& out, const ObjectProps& p);

}  // namespace lazysf2::acmi
