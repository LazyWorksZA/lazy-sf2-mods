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

/// <summary>
/// Parses an Options.ini enumeration value: a whole number in [0, max_value], surrounding blanks allowed. Missing,
/// invalid, fractional or out-of-range text gives -1 (the caller uses its stock value).
/// </summary>
inline int parse_choice_setting(const wchar_t* text, int max_value) {
    const NumberSetting s = parse_number_setting(text, -1.0, -1.0, static_cast<double>(max_value) + 1.0);
    if (s.defaulted || s.value < 0.0 || s.value > max_value || s.value != std::floor(s.value)) return -1;
    return static_cast<int>(s.value);
}

}  // namespace LazyWorks::SF2Patch::config
