#include "acmi/ini.h"

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

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
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
            append_utf8(t, u);
        }
        return t;
    }
    size_t i = 0;
    if (n >= 3 && d[0] == 0xef && d[1] == 0xbb && d[2] == 0xbf) i = 3;  // a UTF-8 BOM: the rest is taken as it is
    const bool utf8 = i == 3;
    t.reserve(n);
    for (; i < n; ++i) {
        if (d[i] < 0x80 || utf8)
            t += static_cast<char>(d[i]);
        else
            append_utf8(t, d[i]);
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
