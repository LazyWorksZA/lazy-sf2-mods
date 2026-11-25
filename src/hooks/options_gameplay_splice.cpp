#include "hooks/options_gameplay_splice.h"

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

}  // namespace

int splice_gameplay_row(std::wstring& text) {
    if (text.find(L"[TacviewRecordingList]") != std::wstring::npos) return 0;
    std::wstring t = text;

    size_t b = 0, e = 0;
    if (!section_range(t, L"GameplayOptionsSubMenu", b, e)) return 0;
    // Highest MenuItem number, and the line after the last MenuItem line (where the row items go).
    int max_id = 0;
    size_t ins = b;
    for (size_t p = t.find(L"MenuItem", b); p != std::wstring::npos && p < e; p = t.find(L"MenuItem", p + 8)) {
        if (p > 0 && t[p - 1] != L'\n') continue;
        int id = 0;
        if (swscanf_s(t.c_str() + p, L"MenuItem%d=", &id) != 1) continue;
        if (id > max_id) max_id = id;
        const size_t eol = t.find(L'\n', p);
        ins = (eol == std::wstring::npos) ? t.size() : eol + 1;
    }
    if (max_id <= 0 || max_id > 997) return 0;
    const int id = max_id + 1;

    wchar_t items[128];
    swprintf_s(items, L"MenuItem%03d=TacviewRecordingList\r\nMenuItem%03d=TacviewRecordingText\r\n", id, id + 1);
    if (ins == t.size() && t.back() != L'\n') {  // last MenuItem line ends the file without a newline
        t += L"\r\n";
        ins = t.size();
    }
    t.insert(ins, items);

    // A full-width row below the last stock rows (y=350), laid out like MissionStartList / MissionStartText.
    if (!t.empty() && t.back() != L'\n') t += L"\r\n";
    t += L"\r\n[TacviewRecordingList]\r\n"
         L"MenuItemType=LIST\r\n"
         L"Position=200,400\r\n"
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
    t += L"\r\nTextPosition=0,403\r\n";
    text = std::move(t);
    return id;
}

}  // namespace lazysf2::hooks
