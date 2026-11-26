// Exports the loaded theatre's heightmap, ground texture, airfields and placed structures for Tacview. See lazy-sf2-recon: recon-notes
// §19.9 and §21.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "acmi/acmi_format.h"
#include "acmi/airfields.h"

namespace LazyWorks::SF2Patch::hooks {

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
    std::wstring theatre_ini;  // the theatre's own ini (its base name): names the types ini
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
    bool has_geo = false;  // the theatre's ini names a real-world point (its centre)
    double geo_lon = 0, geo_lat = 0;
    std::unique_ptr<uint16_t[]> samples;  // w*h, south row first; null when already exported or not copied
    std::unique_ptr<TextureCapture> texture;  // only with samples; null when it could not be read
};

/// <summary>
/// Game thread. Reads the loaded theatre; false when none is loaded or the build or values are unexpected. Copies the
/// samples (and the tile field) only when this id has not been exported this session and no export is running.
/// </summary>
bool terrain_capture(TerrainInfo& out);

/// <summary>
/// Game thread. As terrain_capture, but fills only id, full_name, w, h, spacing and the geo point: no sample or tile
/// copy, no export state touched and no log lines.
/// </summary>
bool terrain_basics(TerrainInfo& out);

/// <summary>
/// Game thread. Hands info.samples to the background export thread, if there are any. The export uses the recorder's
/// placement, so the files and the recording agree.
/// </summary>
void terrain_export_start(TerrainInfo& info, const acmi::Placement& placement);

/// <summary>Waits for a running export for at most 1 s, then detaches it.</summary>
void terrain_export_shutdown();

/// <summary>An airfield for the recording: world metres from the theatre's south-west corner, heading in radians.</summary>
struct AerodromeMark {
    std::string name;
    double x = 0, y = 0, z = 0;
    double heading = 0;
};

enum class StaticList { Pending, Ready, None };

/// <summary>
/// The airfields of theatre id: Pending until the export thread has read them, then Ready (out shares the list) or
/// None. The list is never copied under the lock. Any thread.
/// </summary>
StaticList terrain_aerodromes(const std::string& id, std::shared_ptr<const std::vector<AerodromeMark>>& out);

/// <summary>A placed building or bridge for the recording: world metres, z on the terrain, heading in radians.</summary>
struct StructureMark {
    std::string name;
    double x = 0, y = 0, z = 0;
    double heading = 0;
    bool bridge = false;
};

/// <summary>A theatre's structures by target area: marks[first[i] .. first[i + 1]) are area i's.</summary>
struct StructureList {
    std::vector<acmi::TargetArea> areas;
    std::vector<uint32_t> first;  // areas.size() + 1 entries
    std::vector<StructureMark> marks;
};

/// <summary>The structures of theatre id, published like the airfields (see terrain_aerodromes). Any thread.</summary>
StaticList terrain_structures(const std::string& id, std::shared_ptr<const StructureList>& out);

}  // namespace LazyWorks::SF2Patch::hooks
