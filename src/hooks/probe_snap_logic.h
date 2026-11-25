#pragma once
// Pure naming and retention rules for the probe's automatic snapshots (probe_hooks.cpp), free of Windows headers so
// the tests can use them. Files are snapNNN_auto*.* (NNN: 3 or more digits); manual ones are snapNNN_<id>*.bin and
// snapNNN.txt.
#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

namespace LazyWorks::SF2Patch::hooks {

namespace detail {
inline bool is_lower_hex(const wchar_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!((p[i] >= L'0' && p[i] <= L'9') || (p[i] >= L'a' && p[i] <= L'f'))) return false;
    return true;
}
inline bool is_text(const wchar_t* p, const wchar_t* t) {
    size_t i = 0;
    for (; t[i]; ++i)
        if (p[i] != t[i]) return false;
    return p[i] == L'\0';
}
}  // namespace detail

/// <summary>
/// The snapshot number in a probe file name (snap, 3 to 9 digits, then anything), or -1 if the name is not one of
/// ours. *is_auto is true only for the four shapes the automatic snapshot writes after snapNNN_auto: .txt, _world.txt,
/// _&lt;8 hex&gt;.bin and _&lt;8 hex&gt;_f&lt;4 hex&gt;.bin (lower case). Anything else is left alone.
/// </summary>
inline long probe_snapshot_number(const wchar_t* name, bool* is_auto) {
    *is_auto = false;
    static const wchar_t kPrefix[] = L"snap", kAuto[] = L"_auto";
    size_t i = 0;
    for (; kPrefix[i]; ++i)
        if (name[i] != kPrefix[i]) return -1;
    long n = 0;
    size_t digits = 0;
    while (name[i] >= L'0' && name[i] <= L'9' && digits < 9) {
        n = n * 10 + (name[i] - L'0');
        ++i;
        ++digits;
    }
    if (digits < 3) return -1;
    size_t k = 0;
    for (; kAuto[k]; ++k)
        if (name[i + k] != kAuto[k]) return n;
    const wchar_t* rest = name + i + k;
    if (detail::is_text(rest, L".txt") || detail::is_text(rest, L"_world.txt")) {
        *is_auto = true;
    } else if (rest[0] == L'_' && detail::is_lower_hex(rest + 1, 8)) {
        const wchar_t* tail = rest + 9;
        *is_auto = detail::is_text(tail, L".bin") ||
                   (tail[0] == L'_' && tail[1] == L'f' && detail::is_lower_hex(tail + 2, 4) && detail::is_text(tail + 6, L".bin"));
    }
    return n;
}

/// <summary>The auto snapshot numbers (any order, duplicates allowed) that fall outside the newest keep, highest first.</summary>
inline std::vector<long> probe_auto_numbers_to_delete(std::vector<long> numbers, size_t keep) {
    std::sort(numbers.begin(), numbers.end(), std::greater<long>());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    if (numbers.size() > keep) return std::vector<long>(numbers.begin() + static_cast<std::ptrdiff_t>(keep), numbers.end());
    return {};
}

}  // namespace LazyWorks::SF2Patch::hooks
