#include "acmi/ini.h"
#include "acmi/acmi_format.h"

#include <cstdlib>

namespace LazyWorks::SF2Patch::acmi {

std::string to_lower_ascii(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

const IniSection* Ini::find(std::string_view section) const {
    const std::string want = to_lower_ascii(section);
    for (const IniSection& s : sections)
        if (s.name == want) return &s;
    return nullptr;
}

const std::string* Ini::get(std::string_view section, std::string_view key) const {
    const IniSection* s = find(section);
    if (!s) return nullptr;
    const std::string want = to_lower_ascii(key);
    for (const auto& kv : s->items)
        if (kv.first == want) return &kv.second;
    return nullptr;
}

namespace {

constexpr uint16_t kCp1252High[32] = {
    0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
    0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};

/// <summary>True when every byte of d is ASCII or part of well-formed UTF-8, and there is at least one multi-byte sequence.</summary>
bool is_multibyte_utf8(const uint8_t* d, size_t n) {
    bool multi = false;
    for (size_t i = 0; i < n; ++i) {
        if (d[i] < 0x80) continue;
        const size_t len = acmi::utf8_seq_len(d + i, n - i);
        if (!len) return false;
        multi = true;
        i += len - 1;
    }
    return multi;
}

std::string decode_text(const uint8_t* d, size_t n) {
    std::string t;
    if (n >= 2 && d[0] == 0xff && d[1] == 0xfe) {  // UTF-16LE with a BOM
        t.reserve(n / 2);
        for (size_t i = 2; i + 1 < n; i += 2) {
            uint32_t u = d[i] | (static_cast<uint32_t>(d[i + 1]) << 8);
            if (u >= 0xd800 && u < 0xdc00 && i + 3 < n) {
                const uint32_t lo = d[i + 2] | (static_cast<uint32_t>(d[i + 3]) << 8);
                if (lo >= 0xdc00 && lo < 0xe000) {
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
            }
            acmi::append_utf8_cp(t, u);
        }
        return t;
    }
    size_t i = 0;
    if (n >= 3 && d[0] == 0xef && d[1] == 0xbb && d[2] == 0xbf) i = 3;  // a UTF-8 BOM: the rest is taken as it is
    // Game inis are Windows-1252. A file that is well-formed UTF-8 already is taken as it is, not encoded twice.
    // Per file: cp1252 text that happens to be valid UTF-8 is read as UTF-8, and a mixed file loses its cp1252 characters.
    const bool utf8 = i == 3 || is_multibyte_utf8(d, n);
    t.reserve(n);
    for (; i < n; ++i) {
        if (d[i] < 0x80 || utf8)
            t += static_cast<char>(d[i]);
        else
            acmi::append_utf8_cp(t, d[i] < 0xa0 ? kCp1252High[d[i] - 0x80] : d[i]);
    }
    return t;
}

std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

}  // namespace

Ini parse_ini(const uint8_t* data, size_t n) {
    Ini ini;
    if (!data) return ini;
    const std::string text = decode_text(data, n);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string_view line(text.data() + pos, eol - pos);
        pos = eol + 1;
        const size_t semi = line.find(';');
        if (semi != std::string_view::npos) line = line.substr(0, semi);
        const size_t slashes = line.find("//");
        if (slashes != std::string_view::npos) line = line.substr(0, slashes);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            if (close == std::string_view::npos) continue;
            ini.sections.push_back({to_lower_ascii(trim(line.substr(1, close - 1))), {}});
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || ini.sections.empty()) continue;
        ini.sections.back().items.emplace_back(to_lower_ascii(trim(line.substr(0, eq))),
                                               std::string(trim(line.substr(eq + 1))));
    }
    return ini;
}

bool parse_numbers(const std::string& value, double* out, size_t count) {
    const char* p = value.c_str();
    for (size_t i = 0; i < count; ++i) {
        char* end = nullptr;
        out[i] = std::strtod(p, &end);
        if (end == p) return false;
        p = end;
        while (*p == ' ' || *p == '\t') ++p;
        if (i + 1 < count) {
            if (*p != ',') return false;
            ++p;
        }
    }
    return true;
}

}  // namespace LazyWorks::SF2Patch::acmi
