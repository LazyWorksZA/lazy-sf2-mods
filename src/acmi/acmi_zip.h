// Packs a finished recording into a Tacview-readable "<name>.zip.acmi" (a standard zip with one deflated entry
// "<name>.acmi"). Uses miniz and streams from the file, so a 512 MB recording is never held in memory. Pure Win32 and
// miniz: no game dependencies.
#pragma once

#include <cstdint>

#include <string>

namespace lazysf2::acmi {

/// <summary>
/// Compresses plain (a path ending in ".acmi") to the same path with ".zip.acmi". It writes "<zip>.tmp", reopens it and
/// checks the entry's name, size and CRC against the plain file, renames it into place (never over an existing file),
/// and only then deletes plain. On any failure the plain file is kept, the temp file is removed, and error says why.
/// </summary>
/// <param name="zip_path">The final path on success.</param>
/// <param name="zip_bytes">The zip's size on success.</param>
bool compress_acmi(const std::wstring& plain, std::wstring& zip_path, uint64_t& zip_bytes, std::string& error);

}  // namespace lazysf2::acmi
