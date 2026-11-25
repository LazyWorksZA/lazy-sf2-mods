#include "acmi/tacview_terrain.h"

#include <cstring>

#include "acmi/acmi_format.h"

namespace lazysf2::acmi {

std::string terrain_id(std::wstring_view base_name) {
    if (base_name.empty()) return {};
    std::string id = "LazySF2.";
    for (const wchar_t c : base_name) {
        const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' ||
                        c == L'-';
        id += ok ? static_cast<char>(c) : '_';
    }
    if (id.size() > kTerrainIdMaxChars) id.resize(kTerrainIdMaxChars);
    return id;
}

void terrain_raw_north_first(const uint16_t* south_first, int w, int h, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 2);
    size_t o = 0;
    for (int row = h - 1; row >= 0; --row) {
        const uint16_t* src = south_first + static_cast<size_t>(row) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            out[o++] = static_cast<uint8_t>(src[x] & 0xff);
            out[o++] = static_cast<uint8_t>(src[x] >> 8);
        }
    }
}

namespace {
void corner(std::string& x, const char* tag, double lon, double lat) {
    x += "\t\t\t<";
    x += tag;
    x += ">\n\t\t\t\t<Longitude>" + num(lon, 9) + "</Longitude>\n\t\t\t\t<Latitude>" + num(lat, 9) +
         "</Latitude>\n\t\t\t</";
    x += tag;
    x += ">\n";
}
}  // namespace

std::string terrain_xml(const std::string& id, const std::string& raw_file, int w, int h, double spacing, double ref_lon,
                        double ref_lat) {
    const Mapping m = make_mapping(ref_lat);
    const double west = ref_lon, east = ref_lon + (w - 1) * spacing * m.lon_scale;
    const double south = ref_lat, north = ref_lat + (h - 1) * spacing * m.lat_scale;
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n<Resources>\n\t<CustomHeightmapList>\n";
    x += "\t\t<CustomHeightmap Layer=\"Real World\" Id=\"" + id + "\">\n";
    x += "\t\t\t<File>" + raw_file + "</File>\n\t\t\t<Visible>true</Visible>\n\t\t\t<BigEndian>0</BigEndian>\n";
    x += "\t\t\t<Width>" + std::to_string(w) + "</Width>\n\t\t\t<Height>" + std::to_string(h) + "</Height>\n";
    x += "\t\t\t<AltitudeFactor>1</AltitudeFactor>\n\t\t\t<AltitudeOffset>0</AltitudeOffset>\n";
    x += "\t\t\t<Projection>Quad</Projection>\n";
    corner(x, "BottomLeft", west, south);
    corner(x, "BottomRight", east, south);
    corner(x, "TopRight", east, north);
    corner(x, "TopLeft", west, north);
    x += "\t\t</CustomHeightmap>\n\t</CustomHeightmapList>\n</Resources>\n";
    return x;
}

}  // namespace lazysf2::acmi
