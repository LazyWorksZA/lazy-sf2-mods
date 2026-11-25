#pragma once

#include <string>

// Minimal thread-safe logger. Writes lazysf2.log next to our DLL (the game folder).
namespace LazyWorks::SF2Patch::log {

void open();
void close();
void write(const char* fmt, ...);

/// <summary>
/// Returns path with a leading %USERPROFILE% directory replaced by the literal text "%USERPROFILE%",
/// so log lines don't expose the player's profile path.
/// </summary>
std::wstring log_path(const std::wstring& path);

}  // namespace LazyWorks::SF2Patch::log

#define LOG(...) ::LazyWorks::SF2Patch::log::write(__VA_ARGS__)
