// Writes an RGB image as a JPEG with the Windows Imaging Component (part of Windows; no third-party code).
#pragma once

#include <string>

#include "acmi/terrain_texture.h"

namespace lazysf2::hooks {

/// <summary>
/// Writes img to path (created or overwritten). Call from a thread that has initialised COM. False on any failure;
/// the file may then be partial, so write to a temporary name first.
/// </summary>
bool write_jpeg_wic(const acmi::RGBImage& img, const std::wstring& path, float quality);

}  // namespace lazysf2::hooks
