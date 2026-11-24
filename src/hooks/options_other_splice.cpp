#include "hooks/options_other_splice.h"

#include <cstdio>
#include <cwchar>

namespace lazysf2::hooks {
namespace {

// [begin, end) of a section's body (after its header line, up to the next section's line start).
bool section_range(const std::wstring& t, const std::wstring& name, size_t& begin, size_t& end) {
    const std::wstring header = L"\n[" + name + L"]";
    size_t p = t.find(header);
    if (p == std::wstring::npos) {
        if (t.rfind(L"[" + name + L"]", 0) != 0) return false;
        p = 0;
    } else {
        ++p;
    }
    begin = t.find(L'\n', p);
    if (begin == std::wstring::npos) return false;
    ++begin;
    end = t.find(L"\n[", begin);
    end = (end == std::wstring::npos) ? t.size() : end + 1;
    return true;
}

// Sets "key=value" in a section: replaces an existing line, else adds one at the end of the body.
bool set_key(std::wstring& t, const std::wstring& section, const std::wstring& key, const std::wstring& value) {
    size_t b = 0, e = 0;
    if (!section_range(t, section, b, e)) return false;
    const std::wstring prefix = key + L"=";
    for (size_t p = b; p < e;) {
        size_t eol = t.find(L'\n', p);
        if (eol == std::wstring::npos || eol >= e) eol = e - 1;
        if (t.compare(p, prefix.size(), prefix) == 0) {
            size_t vend = eol;
            if (vend > p && t[vend - 1] == L'\r') --vend;
            t.replace(p + prefix.size(), vend - (p + prefix.size()), value);
            return true;
        }
        p = eol + 1;
    }
    std::wstring line = prefix + value + L"\r\n";
    if (e > b && t[e - 1] != L'\n') line.insert(0, L"\r\n");
    t.insert(e, line);
    return true;
}

bool remove_key(std::wstring& t, const std::wstring& section, const std::wstring& key) {
    size_t b = 0, e = 0;
    if (!section_range(t, section, b, e)) return false;
    const std::wstring prefix = key + L"=";
    for (size_t p = b; p < e;) {
        size_t eol = t.find(L'\n', p);
        if (eol == std::wstring::npos || eol >= e) eol = e - 1;
        if (t.compare(p, prefix.size(), prefix) == 0) {
            t.erase(p, eol + 1 - p);
            return true;
        }
        p = eol + 1;
    }
    return false;
}

}  // namespace

int splice_other_page(std::wstring& text) {
    if (text.find(L"[TacviewRecordingList]") != std::wstring::npos) return 0;
    std::wstring t = text;

    size_t b = 0, e = 0;
    if (!section_range(t, L"OtherOptionsSubMenu", b, e)) return 0;
    int max_id = 0;
    for (size_t p = t.find(L"MenuItem", b); p != std::wstring::npos && p < e; p = t.find(L"MenuItem", p + 8)) {
        if (p > 0 && t[p - 1] != L'\n') continue;
        int id = 0;
        if (swscanf_s(t.c_str() + p, L"MenuItem%d=", &id) == 1 && id > max_id) max_id = id;
    }
    if (max_id <= 0 || max_id > 997) return 0;
    const int id = max_id + 1;

    // The five visible tabs, in order. The Network tab stays hidden.
    static const wchar_t* const kTabs[5] = {L"GameplayOptionsButton", L"GraphicsOptionsButton", L"SoundOptionsButton",
                                            L"ControlOptionsButton", L"OtherOptionsButton"};
    for (int i = 0; i < 5; ++i) {
        if (!set_key(t, kTabs[i], L"TextPosition", std::to_wstring(kTabX[i]) + L"," + std::to_wstring(kTabY))) return 0;
    }
    // The tab reaches its page by order (button ID 7 -> page ID 14), like the stock tabs: DestinationMenu stays empty.
    if (!remove_key(t, L"OtherOptionsButton", L"Active")) return 0;
    if (!set_key(t, L"OtherOptionsButton", L"TextStringID", kOtherTabKey)) return 0;
    if (!set_key(t, L"OtherOptionsButton", L"InfoTextStringID", kOtherTabInfoKey)) return 0;

    // Page geometry like the visible pages.
    if (!set_key(t, L"OtherOptionsSubMenu", L"Position", L"550,180") ||
        !set_key(t, L"OtherOptionsSubMenu", L"Width", L"450") || !set_key(t, L"OtherOptionsSubMenu", L"Height", L"460")) {
        return 0;
    }

    // Row items go after the last MenuItem line (positions moved above; re-find the section).
    if (!section_range(t, L"OtherOptionsSubMenu", b, e)) return 0;
    size_t ins = b;
    for (size_t p = t.find(L"MenuItem", b); p != std::wstring::npos && p < e; p = t.find(L"MenuItem", p + 8)) {
        if (p > 0 && t[p - 1] != L'\n') continue;
        size_t eol = t.find(L'\n', p);
        ins = (eol == std::wstring::npos) ? t.size() : eol + 1;
    }
    wchar_t items[128];
    swprintf_s(items, L"MenuItem%03d=TacviewRecordingList\r\nMenuItem%03d=TacviewRecordingText\r\n", id, id + 1);
    t.insert(ins, items);

    if (!t.empty() && t.back() != L'\n') t += L"\r\n";
    t += L"\r\n[TacviewRecordingList]\r\n"
         L"MenuItemType=LIST\r\n"
         L"Position=200,0\r\n"
         L"Width=240\r\n"
         L"Height=22\r\n"
         L"ListFieldSetting=MediumListField\r\n"
         L"DropDownListSize=10\r\n"
         L"InfoTextSetting=InfoTextSetting\r\n"
         L"InfoTextStringID=";
    t += kTacviewInfoKey;
    t += L"\r\n\r\n[TacviewRecordingText]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=MediumTextLeft\r\n"
         L"TextStringID=";
    t += kTacviewLabelKey;
    t += L"\r\nTextPosition=0,3\r\n";
    text = std::move(t);
    return id;
}

}  // namespace lazysf2::hooks
