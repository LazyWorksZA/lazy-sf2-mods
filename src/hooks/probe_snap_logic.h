#pragma once
// Pure naming and retention rules for the probe's automatic snapshots (probe_hooks.cpp), free of Windows headers so
// the tests can use them. Files are snapNNN_auto*.* (NNN: 3 or more digits); manual ones are snapNNN_<id>*.bin and
// snapNNN.txt.
#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>
/// The snapshot number in a probe file name (snap, 3 or more digits, then anything), or -1 if the name is not one of
/// ours. *is_auto is true only for snapNNN_auto followed by the end of the name, '.' or '_'.
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
    const wchar_t end = name[i + k];
    *is_auto = end == L'\0' || end == L'.' || end == L'_';
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
