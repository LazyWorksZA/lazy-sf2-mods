// Reads a game file (loose or inside a cat) through the exe's own file manager. See lazy-sf2-recon: recon-notes §21.4.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lazysf2::hooks {

constexpr size_t kMaxGameFileBytes = 64u * 1024 * 1024;

/// <summary>
/// Reads full_path (absolute: a loose file first, then the mounted cats by bare name). False when the exe build is not
/// the expected one, the manager is not as expected, or the file is missing or over 64 MB. Safe from a worker thread;
/// never changes the current directory.
/// </summary>
bool read_game_file(const std::wstring& full_path, std::vector<uint8_t>& out);

}  // namespace lazysf2::hooks
