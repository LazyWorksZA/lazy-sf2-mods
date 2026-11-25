#pragma once

#include <string>

// Minimal thread-safe logger. Writes lazysf2.log next to our DLL (the game folder).
namespace lazysf2::log {

void open();
void close();
void write(const char* fmt, ...);

/// <summary>
/// Returns path with a leading %USERPROFILE% directory replaced by the literal text "%USERPROFILE%",
/// so log lines don't expose the player's profile path.
/// </summary>
std::wstring log_path(const std::wstring& path);

}  // namespace lazysf2::log

#define LOG(...) ::lazysf2::log::write(__VA_ARGS__)
