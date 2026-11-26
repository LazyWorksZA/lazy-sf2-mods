// Writes an RGB image as a JPEG with the Windows Imaging Component (part of Windows; no third-party code).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "acmi/terrain_texture.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>
/// Writes img to path (created or overwritten). Call from a thread that has initialised COM. False on any failure;
/// the file may then be partial, so write to a temporary name first.
/// </summary>
bool write_jpeg_wic(const acmi::RGBImage& img, const std::wstring& path, float quality);

/// <summary>
/// A JPEG written a band of rows at a time, so a large image is never held whole. Same thread and file rules as
/// write_jpeg_wic; every COM object is released by finish() or the destructor.
/// </summary>
class JpegStreamWriter {
public:
    JpegStreamWriter();
    ~JpegStreamWriter();
    JpegStreamWriter(const JpegStreamWriter&) = delete;
    JpegStreamWriter& operator=(const JpegStreamWriter&) = delete;

    bool open(const std::wstring& path, int w, int h, float quality);
    /// <summary>Appends rows of RGB8 pixels, w each, top row first.</summary>
    bool write_rows(const uint8_t* rgb, int rows);
    /// <summary>Commits the file once every row has been written.</summary>
    bool finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace LazyWorks::SF2Patch::hooks
