// Writes an RGB image as a JPEG, or an RGBA image as a PNG, with the Windows Imaging Component (part of Windows; no third-party code).
#pragma once

#include <string>

#include "acmi/terrain_texture.h"

namespace lazysf2::hooks {

/// <summary>
/// Writes img to path (created or overwritten). Call from a thread that has initialised COM. False on any failure;
/// the file may then be partial, so write to a temporary name first.
/// </summary>
bool write_jpeg_wic(const acmi::RGBImage& img, const std::wstring& path, float quality);

/// <summary>As write_jpeg_wic, for a 32-bit BGRA PNG with straight alpha.</summary>
bool write_png_wic(const acmi::RGBAImage& img, const std::wstring& path);

}  // namespace lazysf2::hooks
