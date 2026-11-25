// The ground texture for Tacview: decodes the theatre's tile images, composes them into one north-up image and
// paints the runways. Pure: no Windows or game calls. See lazy-sf2-recon: recon-notes §21.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lazysf2::acmi {

constexpr int kMaxImageDim = 4096;
constexpr uint8_t kTarmac[3] = {64, 64, 64};
constexpr double kMinRunwayPixels = 2.0;  // a runway is painted at least this wide

/// <summary>RGBA8, top row first.</summary>
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

/// <summary>RGB8, top row first. For the composed texture, the top row is north.</summary>
struct RGBImage {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

struct RGB {
    uint8_t r = 0, g = 0, b = 0;
};

/// <summary>TGA (type 2 and 10, 16/24/32 bpp) and BMP (8/24/32 bpp), up to 4096 x 4096. False for anything else.</summary>
bool decode_image(const uint8_t* data, size_t n, Image& out);

/// <summary>An n x n box filter. A source smaller than n repeats its pixels.</summary>
void box_downsample(const Image& in, int n, Image& out);

/// <summary>Turns the image 90 degrees clockwise, quarter_turns times (taken modulo 4).</summary>
void rotate_cw(const Image& in, int quarter_turns, Image& out);

/// <summary>
/// Builds the north-up image from tiles_w x tiles_h tiles of n x n pixels. index and rot hold one entry per tile, row 0
/// south (the game's order). A tile whose image is missing or not n x n is filled with its colour.
/// </summary>
void compose(int tiles_w, int tiles_h, int n, const uint16_t* index, const uint8_t* rot,
             const std::vector<Image>& tiles_by_index, const std::vector<RGB>& color_by_index, RGBImage& out);

/// <summary>
/// Paints a runway rectangle. (cx, cy) is in metres, x east and y north, from the south-west corner of the image;
/// heading is clockwise from north in radians.
/// </summary>
void paint_runway(RGBImage& img, double metres_per_px, double cx, double cy, double heading_rad, double length,
                  double width);

/// <summary>A Tacview CustomTextureList with one texture whose corners are the outer edges of the extent.</summary>
std::string texture_xml(const std::string& id, const std::string& file, int w_px, int h_px, double extent_x_m,
                        double extent_y_m, double ref_lon, double ref_lat);

}  // namespace lazysf2::acmi
