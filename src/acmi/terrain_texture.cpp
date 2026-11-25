#include "acmi/terrain_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "acmi/acmi_format.h"
#include "acmi/airfields.h"

namespace lazysf2::acmi {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool size_ok(int w, int h) { return w >= 1 && h >= 1 && w <= kMaxImageDim && h <= kMaxImageDim; }

// One pixel of 2, 3 or 4 bytes as a TGA file stores it: 5-5-5, BGR or BGRA. The 16-bit attribute bit is not alpha.
void put_pixel(uint8_t* dst, const uint8_t* src, size_t bytes) {
    if (bytes == 2) {
        const unsigned v = rd16(src);
        const auto e = [](unsigned c) { return static_cast<uint8_t>((c << 3) | (c >> 2)); };
        dst[0] = e((v >> 10) & 31);
        dst[1] = e((v >> 5) & 31);
        dst[2] = e(v & 31);
        dst[3] = 255;
    } else {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = bytes == 4 ? src[3] : 255;
    }
}

bool decode_tga(const uint8_t* d, size_t n, Image& out) {
    if (n < 18) return false;
    const size_t id_len = d[0];
    const int cmap_type = d[1], type = d[2];
    const size_t cmap_len = rd16(d + 5), cmap_bits = d[7];
    const int w = rd16(d + 12), h = rd16(d + 14), bpp = d[16], desc = d[17];
    if ((type != 2 && type != 10) || cmap_type > 1 || !size_ok(w, h)) return false;
    if (bpp != 16 && bpp != 24 && bpp != 32) return false;
    const size_t bytes = static_cast<size_t>(bpp / 8);
    const size_t cmap_bytes = cmap_type ? cmap_len * ((cmap_bits + 7) / 8) : 0;
    size_t pos = 18 + id_len + cmap_bytes;
    if (pos > n) return false;
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> px(count * 4);  // the only full-size buffer: flipped in place below
    if (type == 2) {
        if ((n - pos) / bytes < count) return false;
        for (size_t i = 0; i < count; ++i) put_pixel(&px[i * 4], d + pos + i * bytes, bytes);
    } else {
        size_t i = 0;
        while (i < count) {
            if (pos >= n) return false;
            const uint8_t head = d[pos++];
            const size_t run = static_cast<size_t>(head & 0x7f) + 1;
            if (run > count - i) return false;
            if (head & 0x80) {
                if (n - pos < bytes) return false;
                uint8_t one[4];
                put_pixel(one, d + pos, bytes);
                pos += bytes;
                for (size_t k = 0; k < run; ++k) std::memcpy(&px[(i + k) * 4], one, 4);
            } else {
                if ((n - pos) / bytes < run) return false;
                for (size_t k = 0; k < run; ++k) put_pixel(&px[(i + k) * 4], d + pos + k * bytes, bytes);
                pos += run * bytes;
            }
            i += run;
        }
    }
    // Stored bottom-up unless bit 0x20; right to left when bit 0x10. Make it top-down, left to right, in place.
    const bool top_down = (desc & 0x20) != 0, right_to_left = (desc & 0x10) != 0;
    const size_t row_bytes = static_cast<size_t>(w) * 4;
    if (!top_down)
        for (int y = 0; y < h / 2; ++y)
            std::swap_ranges(px.begin() + static_cast<std::ptrdiff_t>(y * row_bytes),
                             px.begin() + static_cast<std::ptrdiff_t>((y + 1) * row_bytes),
                             px.begin() + static_cast<std::ptrdiff_t>((h - 1 - y) * row_bytes));
    if (right_to_left) {
        for (int y = 0; y < h; ++y) {
            uint8_t* row = &px[static_cast<size_t>(y) * row_bytes];
            for (int x = 0; x < w / 2; ++x)
                std::swap_ranges(row + x * 4, row + x * 4 + 4, row + (w - 1 - x) * 4);
        }
    }
    out.w = w;
    out.h = h;
    out.px = std::move(px);
    return true;
}

bool decode_bmp(const uint8_t* d, size_t n, Image& out) {
    if (n < 14 + 40 || d[0] != 'B' || d[1] != 'M') return false;
    const uint32_t off_bits = rd32(d + 10), dib = rd32(d + 14);
    if (dib < 40 || 14 + static_cast<size_t>(dib) > n) return false;
    const int32_t w = static_cast<int32_t>(rd32(d + 18)), raw_h = static_cast<int32_t>(rd32(d + 22));
    const int bpp = rd16(d + 28);
    const uint32_t compression = rd32(d + 30), colours_used = rd32(d + 46);
    if (compression != 0 || raw_h == INT32_MIN) return false;
    const bool bottom_up = raw_h > 0;
    const int h = raw_h < 0 ? -raw_h : raw_h;
    if (!size_ok(w, h) || (bpp != 8 && bpp != 24 && bpp != 32)) return false;
    uint8_t palette[256][4] = {};
    if (bpp == 8) {
        const size_t entries = colours_used ? std::min<size_t>(colours_used, 256) : 256;
        const size_t pal_pos = 14 + static_cast<size_t>(dib);
        if (pal_pos + entries * 4 > n) return false;
        for (size_t i = 0; i < entries; ++i) std::memcpy(palette[i], d + pal_pos + i * 4, 4);  // B, G, R, unused
    }
    const uint64_t stride = ((static_cast<uint64_t>(w) * static_cast<uint64_t>(bpp) + 31) / 32) * 4;
    if (static_cast<uint64_t>(off_bits) + stride * static_cast<uint64_t>(h) > n) return false;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        const int sy = bottom_up ? h - 1 - y : y;
        const uint8_t* row = d + off_bits + static_cast<size_t>(stride) * static_cast<size_t>(sy);
        uint8_t* dst = &out.px[static_cast<size_t>(y) * static_cast<size_t>(w) * 4];
        for (int x = 0; x < w; ++x, dst += 4) {
            if (bpp == 8) {
                const uint8_t* c = palette[row[x]];
                dst[0] = c[2];
                dst[1] = c[1];
                dst[2] = c[0];
            } else {
                const int bytes = bpp / 8;
                dst[0] = row[x * bytes + 2];
                dst[1] = row[x * bytes + 1];
                dst[2] = row[x * bytes];
            }
            dst[3] = 255;
        }
    }
    return true;
}

}  // namespace

bool decode_image(const uint8_t* data, size_t n, Image& out) {
    out = Image{};
    if (!data) return false;
    Image img;
    const bool ok = (n >= 2 && data[0] == 'B' && data[1] == 'M') ? decode_bmp(data, n, img) : decode_tga(data, n, img);
    if (!ok) return false;
    out = std::move(img);
    return true;
}

void box_downsample(const Image& in, int n, Image& out) {
    Image res;
    if (n >= 1 && in.w >= 1 && in.h >= 1) {
        res.w = res.h = n;
        res.px.resize(static_cast<size_t>(n) * static_cast<size_t>(n) * 4);
        for (int oy = 0; oy < n; ++oy) {
            const int y0 = static_cast<int>(static_cast<int64_t>(oy) * in.h / n);
            const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(oy + 1) * in.h / n));
            for (int ox = 0; ox < n; ++ox) {
                const int x0 = static_cast<int>(static_cast<int64_t>(ox) * in.w / n);
                const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(ox + 1) * in.w / n));
                uint64_t sum[4] = {};
                for (int y = y0; y < y1; ++y) {
                    const uint8_t* p = &in.px[(static_cast<size_t>(y) * in.w + x0) * 4];
                    for (int x = x0; x < x1; ++x, p += 4)
                        for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
                const uint64_t cnt = static_cast<uint64_t>(y1 - y0) * static_cast<uint64_t>(x1 - x0);
                uint8_t* o = &res.px[(static_cast<size_t>(oy) * n + ox) * 4];
                for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>((sum[c] + cnt / 2) / cnt);
            }
        }
    }
    out = std::move(res);
}

void rotate_cw(const Image& in, int quarter_turns, Image& out) {
    const int r = ((quarter_turns % 4) + 4) % 4;
    Image res;
    res.w = (r & 1) ? in.h : in.w;
    res.h = (r & 1) ? in.w : in.h;
    res.px.resize(static_cast<size_t>(res.w) * static_cast<size_t>(res.h) * 4);
    for (int y = 0; y < res.h; ++y) {
        for (int x = 0; x < res.w; ++x) {
            int sx = x, sy = y;
            switch (r) {
                case 1: sx = y; sy = in.h - 1 - x; break;
                case 2: sx = in.w - 1 - x; sy = in.h - 1 - y; break;
                case 3: sx = in.w - 1 - y; sy = x; break;
                default: break;
            }
            std::memcpy(&res.px[(static_cast<size_t>(y) * res.w + x) * 4],
                        &in.px[(static_cast<size_t>(sy) * in.w + sx) * 4], 4);
        }
    }
    out = std::move(res);
}

void compose(int tiles_w, int tiles_h, int n, const uint16_t* index, const uint8_t* rot,
             const std::vector<Image>& tiles_by_index, const std::vector<RGB>& color_by_index, RGBImage& out) {
    out = RGBImage{};
    if (tiles_w < 1 || tiles_h < 1 || n < 1 || !index || !rot) return;
    out.w = tiles_w * n;
    out.h = tiles_h * n;
    out.px.resize(static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 3);
    for (int ty = 0; ty < tiles_h; ++ty) {
        const int block_row = tiles_h - 1 - ty;  // the game's row 0 is the south edge
        for (int tx = 0; tx < tiles_w; ++tx) {
            const size_t t = static_cast<size_t>(ty) * static_cast<size_t>(tiles_w) + static_cast<size_t>(tx);
            const uint16_t k = index[t];
            const Image* img = k < tiles_by_index.size() ? &tiles_by_index[k] : nullptr;
            if (img && (img->w != n || img->h != n || img->px.size() != static_cast<size_t>(n) * n * 4)) img = nullptr;
            const RGB col = k < color_by_index.size() ? color_by_index[k] : RGB{128, 128, 128};
            const int r = rot[t] & 3;
            for (int y = 0; y < n; ++y) {
                uint8_t* dst =
                    &out.px[(static_cast<size_t>(block_row * n + y) * out.w + static_cast<size_t>(tx) * n) * 3];
                for (int x = 0; x < n; ++x, dst += 3) {
                    if (!img) {
                        dst[0] = col.r;
                        dst[1] = col.g;
                        dst[2] = col.b;
                        continue;
                    }
                    int sx = x, sy = y;  // the source pixel of a tile turned r quarter turns clockwise
                    switch (r) {
                        case 1: sx = y; sy = n - 1 - x; break;
                        case 2: sx = n - 1 - x; sy = n - 1 - y; break;
                        case 3: sx = n - 1 - y; sy = x; break;
                        default: break;
                    }
                    const uint8_t* s = &img->px[(static_cast<size_t>(sy) * n + sx) * 4];
                    dst[0] = s[0];
                    dst[1] = s[1];
                    dst[2] = s[2];
                }
            }
        }
    }
}

void paint_runway(RGBImage& img, double mpp, double cx, double cy, double heading, double length, double width) {
    if (img.w < 1 || img.h < 1 || !(mpp > 0) || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(heading) ||
        !std::isfinite(length) || !std::isfinite(width) || length <= 0 || width <= 0)
        return;
    const double half_len = length / 2, half_wid = std::max(width, kMinRunwayPixels * mpp) / 2;
    const double dx = std::sin(heading), dy = std::cos(heading);  // along the runway
    const double reach = std::hypot(half_len, half_wid);          // bounds the pixels to test
    const double xmin = (cx - reach) / mpp, xmax = (cx + reach) / mpp;
    const double ymin = img.h - (cy + reach) / mpp, ymax = img.h - (cy - reach) / mpp;  // row 0 is north
    if (xmax < 0 || ymax < 0 || xmin > img.w || ymin > img.h) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(xmin))), x1 = std::min(img.w - 1, static_cast<int>(std::ceil(xmax)));
    const int y0 = std::max(0, static_cast<int>(std::floor(ymin))), y1 = std::min(img.h - 1, static_cast<int>(std::ceil(ymax)));
    for (int y = y0; y <= y1; ++y) {
        const double wy = (img.h - (y + 0.5)) * mpp - cy;
        for (int x = x0; x <= x1; ++x) {
            const double wx = (x + 0.5) * mpp - cx;
            const double along = wx * dx + wy * dy, across = wx * dy - wy * dx;
            if (std::fabs(along) <= half_len && std::fabs(across) <= half_wid)
                std::memcpy(&img.px[(static_cast<size_t>(y) * img.w + x) * 3], kTarmac, 3);
        }
    }
}

bool runway_patch(const Airfield& a, RGBAImage& out, double& x0, double& y0, double& side_m) {
    if (a.runways.empty() || !std::isfinite(a.x) || !std::isfinite(a.y)) return false;
    double reach = 0;
    for (const Runway& r : a.runways) {
        if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.heading) || !std::isfinite(r.length) ||
            !std::isfinite(r.width) || r.length <= 0 || r.width <= 0)
            return false;
        reach = std::max(reach, std::hypot(r.x - a.x, r.y - a.y) + std::hypot(r.length / 2, r.width / 2));
    }
    side_m = std::max(kPatchMinSideM, 2 * reach + kPatchMarginM);
    const int n = std::clamp(static_cast<int>(std::ceil(side_m / kPatchTargetMpp)), kPatchMinPx, kPatchMaxPx);
    const double mpp = side_m / n;
    x0 = a.x - side_m / 2;
    y0 = a.y - side_m / 2;
    out.w = out.h = n;
    out.px.assign(static_cast<size_t>(n) * n * 4, 0);
    constexpr uint8_t kAsphalt[4] = {58, 58, 60, 255}, kEdge[4] = {200, 200, 200, 255}, kCentre[4] = {230, 230, 230, 255};
    constexpr double kDash = 30.0, kGap = 20.0;
    // Three passes, so a line never hides under another runway's asphalt.
    for (int pass = 0; pass < 3; ++pass) {
        for (const Runway& r : a.runways) {
            const double half_len = r.length / 2, half_wid = std::max(r.width, 3 * mpp) / 2;
            const double dx = std::sin(r.heading), dy = std::cos(r.heading);
            const double reach_r = std::hypot(half_len, half_wid) + mpp;
            const int c0 = std::max(0, static_cast<int>(std::floor((r.x - reach_r - x0) / mpp)));
            const int c1 = std::min(n - 1, static_cast<int>(std::ceil((r.x + reach_r - x0) / mpp)));
            const int r0 = std::max(0, static_cast<int>(std::floor((y0 + side_m - (r.y + reach_r)) / mpp)));
            const int r1 = std::min(n - 1, static_cast<int>(std::ceil((y0 + side_m - (r.y - reach_r)) / mpp)));
            for (int y = r0; y <= r1; ++y) {
                const double wy = y0 + side_m - (y + 0.5) * mpp - r.y;
                for (int x = c0; x <= c1; ++x) {
                    const double wx = x0 + (x + 0.5) * mpp - r.x;
                    const double along = wx * dx + wy * dy, across = wx * dy - wy * dx;
                    if (std::fabs(along) > half_len || std::fabs(across) > half_wid) continue;
                    const uint8_t* c = nullptr;
                    if (pass == 0) {
                        c = kAsphalt;
                    } else if (pass == 1) {
                        if (std::fabs(across) > half_wid - mpp) c = kEdge;
                    } else if (std::fabs(across) <= mpp / 2 &&
                               std::fmod(along + half_len, kDash + kGap) < kDash) {
                        c = kCentre;
                    }
                    if (c) std::memcpy(&out.px[(static_cast<size_t>(y) * n + x) * 4], c, 4);
                }
            }
        }
    }
    return true;
}

std::string texture_xml(const std::string& id, const std::string& file, int /*w_px*/, int /*h_px*/, double extent_x_m,
                        double extent_y_m, double ref_lon, double ref_lat, double scale_lat,
                        const std::vector<PatchEntry>& patches) {
    // The extent is the tiles' outer edges; the heightmap's corners are sample points, which is a different thing.
    const Mapping m = make_mapping(scale_lat);
    std::string x =
        "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n<Resources>\n\t<CustomTextureList>\n";
    const auto entry = [&](const std::string& eid, const std::string& efile, double ex0, double ey0, double w_m, double h_m) {
        const double west = ref_lon + ex0 * m.lon_scale, east = ref_lon + (ex0 + w_m) * m.lon_scale;
        const double south = ref_lat + ey0 * m.lat_scale, north = ref_lat + (ey0 + h_m) * m.lat_scale;
        x += "\t\t<CustomTexture Layer=\"Real World\" Id=\"" + eid + "\">\n";
        x += "\t\t\t<File>" + efile + "</File>\n\t\t\t<Visible>true</Visible>\n\t\t\t<Projection>Quad</Projection>\n";
        const auto corner = [&](const char* tag, double lon, double lat) {
            x += std::string("\t\t\t<") + tag + ">\n\t\t\t\t<Longitude>" + num(lon, 9) + "</Longitude>\n\t\t\t\t<Latitude>" +
                 num(lat, 9) + "</Latitude>\n\t\t\t</" + tag + ">\n";
        };
        corner("BottomLeft", west, south);
        corner("BottomRight", east, south);
        corner("TopRight", east, north);
        corner("TopLeft", west, north);
        x += "\t\t</CustomTexture>\n";
    };
    entry(id, file, 0, 0, extent_x_m, extent_y_m);
    for (const PatchEntry& p : patches) entry(p.id, p.file, p.x0, p.y0, p.side_m, p.side_m);
    x += "\t</CustomTextureList>\n</Resources>\n";
    return x;
}

}  // namespace lazysf2::acmi
