// The theatre's airfields and placed structures, from its targets ini, types ini and the airfield inis they name. Pure: no Windows or game calls.
// See lazy-sf2-recon: recon-notes §21.5.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "acmi/ini.h"

namespace LazyWorks::SF2Patch::acmi {

constexpr int kMaxRunways = 4;

struct Runway {
    double x = 0, y = 0;     // centre, world metres: x east, y north, from the south-west corner
    double heading = 0;      // radians clockwise from north
    double length = 0, width = 0;
};

struct Airfield {
    std::string name;        // UTF-8
    double x = 0, y = 0;     // the target area's position
    double heading = 0;      // radians: the first runway's, or the area's when there are none
    std::vector<Runway> runways;
};

/// <summary>Returns the bytes of an airfield ini by the name the targets ini gives, or nothing when it cannot be read.</summary>
using AirfieldLoader = std::function<std::vector<uint8_t>(const std::string& name)>;

/// <summary>
/// One entry per [TargetAreaNNN] with an AirfieldDataFile, except carrier stations and off-map bases. An airfield whose
/// ini cannot be read is kept, without runways. The area heading is the Heading of the area's last runway target, as in
/// the game; with types, a runway is a type whose TargetType ends in RUNWAY (otherwise a type id starting "Runway").
/// </summary>
std::vector<Airfield> airfields_from(const Ini& targets, const AirfieldLoader& loader, const Ini* types = nullptr);

/// <summary>A target area that can hold structures, as the targets ini gives it.</summary>
struct TargetArea {
    double x = 0, y = 0;  // world metres
    double radius = 0;    // the larger of its Radius and its farthest placed object
};

/// <summary>A placed object of a target area: a building, bridge or other scenery.</summary>
struct Structure {
    std::string name;      // UTF-8: the type's FullName, or the type id when the types ini does not list it
    double x = 0, y = 0;   // world metres
    double heading = 0;    // radians clockwise from north
    bool bridge = false;
    uint32_t area = 0;     // index into PlacedStructures::areas
};

struct PlacedStructures {
    std::vector<TargetArea> areas;  // every [TargetAreaNNN] except carrier stations and off-map bases, in file order
    std::vector<Structure> items;   // grouped by area, in area order, then by target number
};

/// <summary>
/// Every Target[NNN] of the target areas, in the world frame: Position + R(A)Â·Offset, heading A + Heading. As in the
/// game (SingleMission +0x45420), only an airfield area (AirfieldDataFile) with RotateToRunwayHeading on (the default)
/// turns its targets, and only those after a runway target, by the heading of the latest one; others keep A = 0.
/// Skips runways and the types the types ini marks UseGroundObject (the game makes those live objects).
/// </summary>
PlacedStructures structures_from(const Ini& targets, const Ini& types);

/// <summary>Areas closer than this to a position the game reports are the same area.</summary>
constexpr double kAreaMatchMetres = 1.0;

/// <summary>What decides which target areas' structures are recorded.</summary>
struct AreaPick {
    /// <summary>The game's populated areas (a single mission): when set, exactly those are picked.</summary>
    bool use_active = false;
    const double (*active)[2] = nullptr;
    size_t n_active = 0;
    /// <summary>The fallback: areas within radius + range of the player, or with a live ground object inside them.</summary>
    bool have_player = false;
    double player[2] = {};
    double range = 0;
    const double (*ground)[2] = nullptr;
    size_t n_ground = 0;
};

/// <summary>Sets out[i] to 1 for each picked area (0 otherwise). Returns the number picked.</summary>
size_t pick_areas(const TargetArea* areas, size_t n, const AreaPick& pick, uint8_t* out);

/// <summary>The length of the longest prefix of s, at most max bytes, that does not cut a UTF-8 character.</summary>
size_t utf8_prefix(const std::string& s, size_t max);

/// <summary>
/// The types ini's name: the theatre ini's [Terrain] TargetTypeFile, else the targets ini's name with its "_TARGETS"
/// suffix turned into "_TYPES". Empty when neither gives one.
/// </summary>
std::string types_file_name(const Ini& theatre, const std::string& targets_file);

/// <summary>
/// The height at (x, y) metres from a south-row-first grid of w x h samples spacing metres apart, interpolated between
/// the four nearest samples. 0 when the point is outside the grid or the grid is empty.
/// </summary>
double height_at(const uint16_t* samples, int w, int h, double spacing, double x, double y);

}  // namespace LazyWorks::SF2Patch::acmi
