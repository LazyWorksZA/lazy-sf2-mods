#pragma once

// Minimal thread-safe logger. Writes lazysf2.log next to our DLL (the game folder).
namespace lazysf2::log {

void open();
void close();
void write(const char* fmt, ...);

}  // namespace lazysf2::log

#define LOG(...) ::lazysf2::log::write(__VA_ARGS__)
