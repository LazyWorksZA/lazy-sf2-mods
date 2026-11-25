// The theatre's airfields and placed structures, from its targets ini, types ini and the airfield inis they name. Pure: no Windows or game calls.
// See lazy-sf2-recon: recon-notes §21.5.
#pragma once

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
/// ini cannot be read is kept, without runways.
/// </summary>
std::vector<Airfield> airfields_from(const Ini& targets, const AirfieldLoader& loader);

/// <summary>A placed object of a target area: a building, bridge or other scenery.</summary>
struct Structure {
    std::string name;      // UTF-8: the type's FullName, or the type id when the types ini does not list it
    double x = 0, y = 0;   // world metres
    double heading = 0;    // radians clockwise from north
    bool bridge = false;
};

/// <summary>
/// Every Target[NNN] of every [TargetAreaNNN], except in carrier stations and off-map bases, turned into the world
/// frame like the runways (Position + R(A)·Offset, heading A + Heading, A only with RotateToRunwayHeading, default on).
/// Skips runways and the types the types ini marks UseGroundObject (the game makes those live objects).
/// </summary>
std::vector<Structure> structures_from(const Ini& targets, const Ini& types);

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
