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

constexpr double kPatchMarginM = 400.0, kPatchMinSideM = 2000.0;
constexpr int kPatchTileMaxPx = 512, kPatchTileMinPx = 64, kPatchMaxSidePx = 2048;
constexpr size_t kPatchCacheBytes = 64u * 1024u * 1024u;  // the decoded tiles the patches need, as RGB

/// <summary>An airfield patch: a rectangle of whole tiles (tx0, ty0 from the south-west tile) and its world extent.</summary>
struct PatchTiles {
    int tx0 = 0, ty0 = 0, nx = 0, ny = 0;
    double x0 = 0, y0 = 0, w_m = 0, h_m = 0;  // south-west corner and size in metres (tile edges)
};

/// <summary>
/// The tiles covering all the airfield's runways plus a margin (at least 2000 m across), snapped outward to whole tiles
/// and clamped to the tile field. False when there are no usable runways or nothing of the square is inside the field.
/// </summary>
bool patch_tiles(const Airfield& a, double tile_m, int tiles_w, int tiles_h, PatchTiles& out);

/// <summary>
/// The pixel size of a cached tile: min(native, 512), halved (down to 64) until texture_count tiles fit budget_bytes as RGB.
/// </summary>
int patch_tile_px(int native, size_t texture_count, size_t budget_bytes);

/// <summary>
/// Composes the patch's tiles, n x n pixels each, from the theatre's tile field (tiles_w wide, row 0 south): the same
/// as compose for that region.
/// </summary>
void compose_patch(const PatchTiles& pt, int tiles_w, int n, const uint16_t* index, const uint8_t* rot,
                   const std::vector<Image>& tiles_by_index, const std::vector<RGB>& color_by_index, RGBImage& out);

/// <summary>The size a w x h patch is shrunk to so that neither side is over kPatchMaxSidePx. Unchanged when it fits.</summary>
void fit_patch_size(int w, int h, int& out_w, int& out_h);

/// <summary>A box resize of an RGB image to w x h, each at most the source size.</summary>
void resize_box_rgb(const RGBImage& in, int w, int h, RGBImage& out);

/// <summary>
/// Draws the airfield's runways (asphalt, edge lines, dashed centre line) on img, whose south-west corner is (x0, y0)
/// metres and which spans w_m x h_m metres.
/// </summary>
void draw_runways(RGBImage& img, const Airfield& a, double x0, double y0, double w_m, double h_m);

/// <summary>An extra texture over the base one: a rectangle of w_m x h_m metres whose south-west corner is (x0, y0).</summary>
struct PatchEntry {
    std::string id, file;
    double x0 = 0, y0 = 0, w_m = 0, h_m = 0;
};

/// <summary>
/// A Tacview CustomTextureList with the base texture, whose corners are the outer edges of the extent, then one entry
/// per patch.
/// </summary>
std::string texture_xml(const std::string& id, const std::string& file, int w_px, int h_px, double extent_x_m,
                        double extent_y_m, double ref_lon, double ref_lat, double scale_lat,
                        const std::vector<PatchEntry>& patches = {});

}  // namespace lazysf2::acmi
