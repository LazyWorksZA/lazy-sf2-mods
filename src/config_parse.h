// Parsing of numeric Options.ini values (pure; unit-tested).
#pragma once

#include <cmath>
#include <cwchar>

namespace LazyWorks::SF2Patch::config {

/// <summary>A numeric setting as read: the value to use, and whether the text was missing/invalid or out of range.</summary>
struct NumberSetting {
    double value;
    bool defaulted;  // missing, empty or not a number: the default is used
    bool clamped;    // a number outside [lo, hi]: the nearest limit is used
};

/// <summary>
/// Parses text (an Options.ini value, null or empty when missing) as a decimal number, surrounding blanks allowed and
/// nothing else after it. Missing or invalid gives def; a number outside [lo, hi] is clamped to it.
/// </summary>
inline NumberSetting parse_number_setting(const wchar_t* text, double def, double lo, double hi) {
    if (!text) return {def, true, false};
    while (*text == L' ' || *text == L'\t') ++text;
    if (!*text) return {def, true, false};
    wchar_t* end = nullptr;
    const double v = std::wcstod(text, &end);
    if (end == text) return {def, true, false};
    while (*end == L' ' || *end == L'\t') ++end;
    if (*end || !std::isfinite(v)) return {def, true, false};
    if (v < lo) return {lo, false, true};
    if (v > hi) return {hi, false, true};
    return {v, false, false};
}

}  // namespace LazyWorks::SF2Patch::config
