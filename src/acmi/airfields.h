// The theatre's airfields, from its targets ini and the airfield inis they name. Pure: no Windows or game calls.
// See lazy-sf2-recon: recon-notes §21.5.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "acmi/ini.h"

namespace lazysf2::acmi {

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

/// <summary>
/// The height at (x, y) metres from a south-row-first grid of w x h samples spacing metres apart, interpolated between
/// the four nearest samples. 0 when the point is outside the grid or the grid is empty.
/// </summary>
double height_at(const uint16_t* samples, int w, int h, double spacing, double x, double y);

}  // namespace lazysf2::acmi
