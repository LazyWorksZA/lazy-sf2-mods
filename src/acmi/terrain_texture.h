// The ground texture for Tacview: decodes the theatre's tile images, composes them into one north-up image and
// paints the runways. Pure: no Windows or game calls. See lazy-sf2-recon: recon-notes §21.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "acmi/airfields.h"

namespace lazysf2::acmi {

constexpr int kMaxImageDim = 2048;
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

/// <summary>TGA (type 2 and 10, 16/24/32 bpp) and BMP (8/24/32 bpp), up to 2048 x 2048. False for anything else.</summary>
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

/// <summary>RGBA8, top row first (row 0 is north). Alpha 0 is fully transparent.</summary>
struct RGBAImage {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

constexpr double kPatchMarginM = 400.0, kPatchMinSideM = 2000.0, kPatchTargetMpp = 8.0;
constexpr int kPatchMinPx = 128, kPatchMaxPx = 1024;

/// <summary>
/// Draws the airfield's runways (asphalt, edge lines, dashed centre line) on a transparent square centred on the
/// airfield. The square's south-west corner is (x0, y0) metres and its side side_m. False when there are no runways
/// or the numbers are not usable.
/// </summary>
bool runway_patch(const Airfield& a, RGBAImage& out, double& x0, double& y0, double& side_m);

/// <summary>An extra texture over the base one: a square of side_m metres whose south-west corner is (x0, y0).</summary>
struct PatchEntry {
    std::string id, file;
    double x0 = 0, y0 = 0, side_m = 0;
};

/// <summary>
/// A Tacview CustomTextureList with the base texture, whose corners are the outer edges of the extent, then one entry
/// per patch.
/// </summary>
std::string texture_xml(const std::string& id, const std::string& file, int w_px, int h_px, double extent_x_m,
                        double extent_y_m, double ref_lon, double ref_lat, const std::vector<PatchEntry>& patches = {});

}  // namespace lazysf2::acmi
