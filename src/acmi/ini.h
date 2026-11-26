// A small ini reader for the game's own files. Pure: no Windows or game calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace LazyWorks::SF2Patch::acmi {

struct IniSection {
    std::string name;  // lower case
    std::vector<std::pair<std::string, std::string>> items;  // key (lower case), value (trimmed), in file order
};

struct Ini {
    std::vector<IniSection> sections;
    /// <summary>The first section with this name (case-insensitive), or null.</summary>
    const IniSection* find(std::string_view section) const;
    /// <summary>The first value of key in section (both case-insensitive), or null.</summary>
    const std::string* get(std::string_view section, std::string_view key) const;
};

/// <summary>
/// Parses UTF-16LE with a byte-order mark (the stock files) or 8-bit text (mod files; bytes above 0x7f are read as
/// Windows-1252). Values come out as UTF-8. ';' and "//" start a comment.
/// </summary>
Ini parse_ini(const uint8_t* data, size_t n);

/// <summary>Reads a comma-separated list of numbers; false if there are fewer than count or one is not a number.</summary>
bool parse_numbers(const std::string& value, double* out, size_t count);

std::string to_lower_ascii(std::string_view s);

}  // namespace LazyWorks::SF2Patch::acmi
