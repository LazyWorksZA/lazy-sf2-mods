// Tacview custom-heightmap export of a game theatre: id, raw file and XML text. Pure: no Windows or game calls.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lazysf2::acmi {

constexpr size_t kTerrainIdMaxChars = 48;

/// <summary>"LazySF2." plus the base name with anything but ASCII letters, digits, '_' and '-' mapped to '_'; capped at 48 chars. Empty in, empty out.</summary>
std::string terrain_id(std::wstring_view base_name);

/// <summary>The id as a file stem: dots become dashes, because Tacview cannot load a texture whose name has more than one dot.</summary>
std::string terrain_file_stem(const std::string& id);

/// <summary>Little-endian u16 raw, north row first: the game keeps the south row first, so the rows are reversed.</summary>
void terrain_raw_north_first(const uint16_t* south_first, int w, int h, std::vector<uint8_t>& out);

/// <summary>
/// A Tacview CustomHeightmapList for one raw file. The corners use the recorder's flat mapping from (ref_lon, ref_lat) with the
/// longitude scale taken at scale_lat, the south-west sample being at the reference point.
/// </summary>
std::string terrain_xml(const std::string& id, const std::string& raw_file, int w, int h, double spacing, double ref_lon,
                        double ref_lat, double scale_lat);

}  // namespace lazysf2::acmi
