// ACMI 2.2 text formatting shared by the offline converter (tools/acmi) and the in-game Tacview recorder
// (src/acmi/tacview_core.*). Pure: no Windows or game dependencies. Spec: Tacview "ACMI telemetry file format".
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace LazyWorks::SF2Patch::acmi {

constexpr double kMetresPerDegree = 111320.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

/// <summary>The local flat mapping: metres east/north of the reference point to degrees of longitude/latitude offset.</summary>
struct Mapping {
    double lon_scale = 0;
    double lat_scale = 0;
};
Mapping make_mapping(double ref_lat_deg);

/// <summary>Where a theatre's local frame sits: the south-west corner, and the latitude the longitude scale uses.</summary>
struct Placement {
    double ref_lon = 0;
    double ref_lat = 0;
    double scale_lat = 0;
};

/// <summary>The open-Atlantic default frame used when the theatre has no real-world position.</summary>
constexpr double kDefaultRefLon = -25.0;
constexpr double kDefaultRefLat = -4.5;

/// <summary>
/// True when a theatre's ini gave a real position: finite, longitude within +-180, latitude within +-85, and not the
/// game's own default (-96.8, 30.2) within 1e-6.
/// </summary>
bool geo_is_set(double lon, double lat);

/// <summary>
/// With geo, (lon, lat) is the centre of the extent_x_m by extent_y_m square and the corner is placed from it; without,
/// the default frame. The corner is clamped to +-180 / +-89 so the header and the XMLs always agree. The inputs must be
/// finite.
/// </summary>
Placement place_theatre(bool has_geo, double lon, double lat, double extent_x_m, double extent_y_m);

/// <summary>Fixed decimals with trailing zeros trimmed (smaller files); "-0" becomes "0".</summary>
std::string num(double v, int decimals);
/// <summary>Degrees into [0, 360); a value that would print as 360.00 becomes 0, and -0.0 becomes +0.0.</summary>
double norm_deg(double d);
/// <summary>
/// Appends s with ',' escaped by a backslash (spec, "Escaping"). Control characters and invalid UTF-8 bytes become '?',
/// so a value can never end a line. Valid UTF-8 (ACMI text is UTF-8) passes through.
/// </summary>
void append_escaped(std::string& out, const char* s);
/// <summary>The byte length (2 to 4) of the well-formed UTF-8 multi-byte sequence at s, or 0 when there is none (no overlongs, surrogates or values above U+10FFFF).</summary>
size_t utf8_seq_len(const unsigned char* s, size_t avail);
/// <summary>Appends code point cp as UTF-8. A surrogate or out-of-range value becomes '?'.</summary>
void append_utf8_cp(std::string& out, uint32_t cp);

/// <summary>"FileType/FileVersion" lines and the global object (id 0). reference_time is e.g. 2026-11-24T20:15:00Z. A non-empty theater adds Theater=.</summary>
void append_header(std::string& out, const char* reference_time, double ref_lon, double ref_lat, const char* title,
                   const char* theater = nullptr);
/// <summary>"#seconds.mmm\n" for a time in whole milliseconds since the start (never negative).</summary>
void append_frame(std::string& out, long long ms);
/// <summary>"-id\n"</summary>
void append_removal(std::string& out, uint32_t id);
/// <summary>
/// A lock line: "id,LockedTarget=target,LockedTargetMode=1\n", or "id,LockedTargetMode=0,LockedTarget=\n" for
/// target 0 (the lock is lost). Ids in hex.
/// </summary>
void append_lock(std::string& out, uint32_t id, uint32_t target);
/// <summary>"0,Event=Destroyed|id|\n": a global event naming one object (hex id).</summary>
void append_event_destroyed(std::string& out, uint32_t id);
/// <summary>
/// "id,T=Lon|Lat|Alt|Roll|Pitch|Yaw|U|V|Heading" without the newline (the caller may append properties first).
/// pos is (east, north, up) in metres, ang is (pitch, heading, roll) in radians as the game keeps them:
/// pitch nose-up positive, heading clockwise from north, roll right-wing-down positive. Yaw and heading are equal,
/// because the flat world is aligned with north.
/// </summary>
void append_transform(std::string& out, uint32_t id, const double pos[3], const float ang[3], const Mapping& m);

/// <summary>The nine formatted components of a 9-field "T=" transform, in file order. Never empty once filled.</summary>
struct TransformText {
    std::string f[9];
};
/// <summary>Formats the nine components exactly as append_transform writes them.</summary>
void format_transform(TransformText& out, const double pos[3], const float ang[3], const Mapping& m);
/// <summary>
/// Delta form (spec: components that did not change since the last value may be omitted). Writes "id,T=a|b|..."
/// with every '|' kept and a component left empty when its text equals last's; then copies cur into last. With
/// first set, or when last is empty, every component is written. Returns false and writes nothing when no component
/// changed (and first is false). The line has no newline.
/// </summary>
bool append_transform_delta(std::string& out, uint32_t id, const TransformText& cur, TransformText& last, bool first);

/// <summary>"id,T=Lon|Lat|Alt" without the newline: a position only (explosions).</summary>
void append_position(std::string& out, uint32_t id, const double pos[3], const Mapping& m);

/// <summary>Properties written after the first transform of an object. A null or empty member is left out.</summary>
struct ObjectProps {
    const char* name = nullptr;
    const char* pilot = nullptr;
    const char* type = nullptr;       // e.g. "Air+FixedWing"
    const char* coalition = nullptr;  // e.g. "Allies"
    const char* color = nullptr;      // e.g. "Blue"
    const char* parent = nullptr;     // the parent object's id in hex, e.g. "10003" (weapons: the shooter)
};
void append_props(std::string& out, const ObjectProps& p);

}  // namespace LazyWorks::SF2Patch::acmi
