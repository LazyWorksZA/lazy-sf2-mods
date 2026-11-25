// Exports the loaded theatre's heightmap, ground texture and airfields for Tacview. See lazy-sf2-recon: recon-notes
// §19.9 and §21.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lazysf2::hooks {

/// <summary>Where the 1000 km theatre square sits in the recording: open Atlantic, clear of real geography.</summary>
constexpr double kTerrainRefLon = -25.0;
constexpr double kTerrainRefLat = -4.5;

/// <summary>One of the theatre's tile textures, as the terrain engine holds it.</summary>
struct TextureEntry {
    float color[3] = {};  // 0..1: what the texture looks like from far away
    bool used = false;    // the engine's flag: some tile uses it
    bool has_water = false;
};

/// <summary>The terrain engine's tile field and texture list, copied on the game thread for the export thread.</summary>
struct TextureCapture {
    std::wstring folder;       // the theatre's folder: the files are read from here (absolute)
    std::wstring targets_ini;  // bare names
    std::wstring data_ini;
    int tiles_w = 0, tiles_h = 0;  // 0 when the tile field was not usable: no texture, but airfields still work
    double tile_m = 0;             // a tile's side in metres
    std::vector<uint16_t> index;   // per tile, row 0 south
    std::vector<uint8_t> rot;
    std::vector<TextureEntry> entries;
};

struct TerrainInfo {
    std::string id;         // the Tacview terrain id, also the Theater= value
    std::string full_name;  // TerrainFullName, narrowed to ASCII
    int w = 0, h = 0;
    double spacing = 0;
    std::unique_ptr<uint16_t[]> samples;  // w*h, south row first; null when already exported or not copied
    std::unique_ptr<TextureCapture> texture;  // only with samples; null when it could not be read
};

/// <summary>
/// Game thread. Reads the loaded theatre; false when none is loaded or the build or values are unexpected. Copies the
/// samples (and the tile field) only when this id has not been exported this session and no export is running.
/// </summary>
bool terrain_capture(TerrainInfo& out);

/// <summary>Game thread. Hands info.samples to the background export thread, if there are any.</summary>
void terrain_export_start(TerrainInfo& info);

/// <summary>Waits for a running export for at most 1 s, then detaches it.</summary>
void terrain_export_shutdown();

/// <summary>An airfield for the recording: world metres from the theatre's south-west corner, heading in radians.</summary>
struct AerodromeMark {
    std::string name;
    double x = 0, y = 0, z = 0;
    double heading = 0;
};

enum class AerodromeList { Pending, Ready, None };

/// <summary>
/// The airfields of theatre id: Pending until the export thread has read them, then Ready (out shares the list) or
/// None. The list is never copied under the lock. Any thread.
/// </summary>
AerodromeList terrain_aerodromes(const std::string& id, std::shared_ptr<const std::vector<AerodromeMark>>& out);

}  // namespace lazysf2::hooks
