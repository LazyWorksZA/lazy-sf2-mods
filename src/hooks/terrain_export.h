// Exports the loaded theatre's heightmap for Tacview. See lazy-sf2-recon: recon-notes §19.9.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace lazysf2::hooks {

/// <summary>Where the 1000 km theatre square sits in the recording: open Atlantic, clear of real geography.</summary>
constexpr double kTerrainRefLon = -25.0;
constexpr double kTerrainRefLat = -4.5;

struct TerrainInfo {
    std::string id;         // the Tacview terrain id, also the Theater= value
    std::string full_name;  // TerrainFullName, narrowed to ASCII
    int w = 0, h = 0;
    double spacing = 0;
    std::unique_ptr<uint16_t[]> samples;  // w*h, south row first; null when already exported or not copied
};

/// <summary>
/// Game thread. Reads the loaded theatre; false when none is loaded or the build or values are unexpected. Copies the
/// samples only when this id has not been exported this session and no export is running.
/// </summary>
bool terrain_capture(TerrainInfo& out);

/// <summary>Game thread. Hands info.samples to the background export thread, if there are any.</summary>
void terrain_export_start(TerrainInfo& info);

/// <summary>Waits for a running export for at most 1 s, then detaches it.</summary>
void terrain_export_shutdown();

}  // namespace lazysf2::hooks
