#include "hooks/lobby_splice.h"

#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <utility>
#include <vector>

namespace LazyWorks::SF2Patch::hooks {
namespace {

std::wstring lower(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

// [begin, end) of a section's body: after its header line, up to the start of the next section's line.
bool section_range(const std::wstring& t, const std::wstring& name, size_t& begin, size_t& end) {
    const std::wstring header = L"[" + name + L"]";
    size_t p = 0;
    for (p = t.find(header); p != std::wstring::npos && p > 0 && t[p - 1] != L'\n'; p = t.find(header, p + 1)) {
    }
    if (p == std::wstring::npos) return false;
    begin = t.find(L'\n', p);
    if (begin == std::wstring::npos) return false;
    ++begin;
    end = t.find(L"\n[", begin);
    end = (end == std::wstring::npos) ? t.size() : end + 1;
    return true;
}

// Sets key=value in the section body [begin, end): replaces an existing value, else adds the line at the end.
void set_key(std::wstring& t, size_t begin, size_t& end, const std::wstring& key, const std::wstring& value) {
    const std::wstring prefix = lower(key) + L"=";
    for (size_t p = begin; p < end;) {
        size_t eol = t.find(L'\n', p);
        if (eol == std::wstring::npos || eol > end) eol = end;
        if (lower(t.substr(p, prefix.size())) == prefix) {
            const size_t v = p + prefix.size();
            size_t vend = t.find_first_of(L"\r\n", v);
            if (vend == std::wstring::npos || vend > end) vend = end;
            t.replace(v, vend - v, value);
            end = end - (vend - v) + value.size();
            return;
        }
        p = eol + 1;
    }
    std::wstring line = key + L"=" + value + L"\r\n";
    if (end > 0 && t[end - 1] != L'\n') line.insert(0, L"\r\n");
    t.insert(end, line);
    end += line.size();
}

std::wstring pos(int x, int y) { return std::to_wstring(x) + L"," + std::to_wstring(y); }

// Builders for the ini sections; every item section ends with a blank line.
struct Ini {
    std::wstring t;
    void line(const std::wstring& key, const std::wstring& value) { t += key + L"=" + value + L"\r\n"; }
    void screen(const wchar_t* name, const wchar_t* background, const std::vector<std::wstring>& items,
                const std::wstring& dll) {
        t += L"[" + std::wstring(name) + L"]\r\n";
        line(L"MenuType", L"NORMAL_MENU");
        line(L"BackgroundImage", background);
        int n = 0;
        wchar_t key[16];
        for (const std::wstring& item : items) {
            swprintf_s(key, L"MenuItem%03d", ++n);
            line(key, item);
        }
        line(L"MenuCodeDLL", dll);
        line(L"RememberMenu", L"TRUE");
        line(L"DefaultSelection", L"-1");
        line(L"MouseAnimation", L"MouseAnimation");
        line(L"BackgroundMusic", L"");
        line(L"TextFilename", L"NetworkConnectScreen.str");
        t += L"\r\n";
    }
    void button(const wchar_t* name, const wchar_t* dest, const wchar_t* style, const wchar_t* id, int x, int y,
                const wchar_t* info) {
        t += L"[" + std::wstring(name) + L"]\r\n";
        line(L"MenuItemType", L"TEXTBUTTON");
        line(L"DestinationMenu", dest);
        line(L"TextSetting", style);
        line(L"ButtonSetting", L"MenuButtonSetting");
        line(L"TextStringID", id);
        line(L"TextPosition", pos(x, y));
        line(L"InfoTextSetting", L"InfoTextSetting");
        line(L"InfoTextStringID", info);
        t += L"\r\n";
    }
    void text(const wchar_t* name, const wchar_t* style, const wchar_t* id, int x, int y) {
        t += L"[" + std::wstring(name) + L"]\r\n";
        line(L"MenuItemType", L"TEXT");
        line(L"TextSetting", style);
        line(L"TextStringID", id);
        if (x >= 0) line(L"TextPosition", pos(x, y));
        t += L"\r\n";
    }
    void edit(const wchar_t* name, int x, int y, int chars, const wchar_t* info) {
        t += L"[" + std::wstring(name) + L"]\r\n";
        line(L"MenuItemType", L"EDIT");
        line(L"Position", pos(x, y));
        line(L"Width", L"280");
        line(L"Height", L"22");
        line(L"EditFieldSetting", L"EditFieldSetting");
        line(L"TextBufferSize", std::to_wstring(chars + 1));
        line(L"NumberOnly", L"FALSE");
        line(L"TabSelectable", L"TRUE");
        line(L"DefaultTextStringID", L"");
        line(L"InfoTextSetting", L"InfoTextSetting");
        line(L"InfoTextStringID", info);
        t += L"\r\n";
    }
    void area(const wchar_t* name, int x, int y, int w, int h, bool scroll) {
        t += L"[" + std::wstring(name) + L"]\r\n";
        line(L"MenuItemType", L"TEXTAREA");
        line(L"TextSetting", L"MediumTextLeft");
        line(L"ScrollBarSetting", scroll ? L"ScrollBarSetting" : L"");
        line(L"Position", pos(x, y));
        line(L"Width", std::to_wstring(w));
        line(L"Height", std::to_wstring(h));
        line(L"Spacing", L"2");
        t += L"\r\n";
    }
};

// The host and join screens: the Roster background's wide panel (x 88-938, header rule at y 146), mission on the
// left, pilots on the right (one TEXT per cell: run-time text needs TEXT items, see kLobbyBlankKey), the session's own
// buttons in the bottom bar like Exit and Fly on the stock screens.
void session_screen(Ini& ini, bool host, const std::wstring& dll) {
    const std::wstring p = host ? L"LazyHost" : L"LazyJoin";
    auto n = [&](const std::wstring& s) { return p + s; };
    std::vector<std::wstring> names = {n(L"LeaveButton"), n(L"ActionButton"), n(L"MissionButton"), n(L"Title"),
                                       n(L"Header"),      n(L"MissionLabel"), n(L"Summary1"),      n(L"Summary2"),
                                       n(L"Summary3"),    n(L"PilotsLabel"),  n(L"SeatHead"),      n(L"CallsignHead"),
                                       n(L"StatusHead"),  n(L"Status"),       n(L"YouFly")};
    for (int r = 0; r < net::kLobbyRows; ++r) {
        const std::wstring k = std::to_wstring(r + 1);
        for (const wchar_t* c : {L"Own", L"Other", L"Open"}) names.push_back(n(L"RowLabel" + k + c));
        names.push_back(n(L"RowButton" + k));
        for (const wchar_t* c : {L"Own", L"Other", L"Open"}) names.push_back(n(L"RowStatus" + k + c));
    }
    ini.screen(host ? kLobbyHostScreen : kLobbyJoinScreen, L"RosterScreen.JPG", names, dll);
    auto at = [&](int item) { return names[item - 1].c_str(); };

    ini.button(at(session_item::kLeave), kLobbyScreen, L"LargerTextLeft",
               host ? L"TXT_LAZY_LOBBY_CLOSE" : L"TXT_LAZY_LOBBY_LEAVE", 48, 694,
               host ? L"TXT_LAZY_LOBBY_CLOSE_INFO" : L"TXT_LAZY_LOBBY_LEAVE_INFO");
    ini.button(at(session_item::kAction), L"", L"LargerTextRight",
               host ? L"TXT_LAZY_LOBBY_LAUNCH" : L"TXT_LAZY_LOBBY_READY", 976, 694,
               host ? L"TXT_LAZY_LOBBY_LAUNCH_INFO" : L"TXT_LAZY_LOBBY_READY_INFO");
    ini.button(at(session_item::kMissionButton), host ? L"SingleMissionScreen" : L"HangarScreen", L"LargerTextCenter",
               host ? L"TXT_LAZY_LOBBY_SETUP" : L"TXT_LAZY_LOBBY_BRIEFING", 298, 575,
               host ? L"TXT_LAZY_LOBBY_SETUP_INFO" : L"TXT_LAZY_LOBBY_BRIEFING_INFO");
    ini.text(at(4), L"LazyLobbyTitleText", L"TXT_MENU_MULTIPLAYER_TITLE", -1, -1);
    ini.text(at(session_item::kHeader), L"LargerTextCenter", kLobbyBlankKey, 513, 120);
    ini.text(at(6), L"LargerTextLeft", host ? L"TXT_LAZY_LOBBY_MISSION" : L"TXT_LAZY_LOBBY_HOST_MISSION", 108, 172);
    for (int i = 0; i < 3; ++i) ini.text(at(session_item::kSummary + i), L"MediumTextLeft", kLobbyBlankKey, 108, 200 + 22 * i);
    ini.text(at(10), L"LargerTextLeft", L"TXT_LAZY_LOBBY_PILOTS", 528, 172);
    ini.text(at(11), L"MediumTextLeft", L"TXT_LAZY_LOBBY_SEAT_CALLSIGN", 528, 192);
    ini.text(at(12), L"MediumTextLeft", kLobbyBlankKey, 600, 192);
    ini.text(at(13), L"MediumTextLeft", L"TXT_LAZY_LOBBY_COL_STATUS", 800, 192);
    ini.text(at(session_item::kStatus), L"MediumTextLeft", kLobbyBlankKey, 108, 612);
    ini.text(at(session_item::kYouFly), L"MediumTextLeft", kLobbyBlankKey, 108, 532);
    // Each row's seat and callsign is a button in the rows' own text style (it lights up under the pointer, as the
    // stock buttons do); its text is set at run time (lobby_screen.cpp, through the button's text object).
    for (int r = 0; r < net::kLobbyRows; ++r) {
        const int y = 214 + 22 * r;
        static const wchar_t* const kStyles[3] = {L"LazyRowOwnText", L"LazyRowOtherText", L"LazyRowOpenText"};
        for (int c = 0; c < 3; ++c) ini.text(at(session_item::row_label(r, c)), kStyles[c], kLobbyBlankKey, 528, y);
        ini.button(at(session_item::row_button(r)), L"", L"MediumTextLeft", session_item::kLobbyRowBlankKey, 528, y, L"TXT_LAZY_LOBBY_ROW_INFO");
        for (int c = 0; c < 3; ++c) ini.text(at(session_item::row_status(r, c)), kStyles[c], kLobbyBlankKey, 800, y);
    }
}

}  // namespace

bool splice_main_screen_button(std::wstring& text) {
    std::wstring t = text;
    size_t b = 0, e = 0;
    if (!section_range(t, L"MultiplayerHotspot", b, e)) return false;
    if (lower(t.substr(b, e - b)).find(L"destinationmenu=" + lower(kLobbyScreen)) == std::wstring::npos) return false;
    set_key(t, b, e, L"Active", L"TRUE");
    set_key(t, b, e, L"Enabled", L"TRUE");
    // In the large style of the three above it, one more of their steps (74) below Campaign (790,404); Pilot Record
    // and Options move down so their spacing stays even above EXIT, which stays at (894,654).
    set_key(t, b, e, L"TextSetting", L"MainMenuText");
    set_key(t, b, e, L"TextPosition", L"790,478");
    for (const auto& [name, pos] : {std::pair{L"PilotRecordHotspot", L"790,545"}, std::pair{L"OptionsHotspot", L"790,600"}}) {
        if (section_range(t, name, b, e)) set_key(t, b, e, L"TextPosition", pos);
    }
    text = std::move(t);
    return true;
}

bool splice_single_mission_for_lobby(std::wstring& text) {
    std::wstring t = text;
    size_t b = 0, e = 0;
    if (!section_range(t, L"CancelButton", b, e)) return false;
    set_key(t, b, e, L"DestinationMenu", kLobbyHostScreen);
    // The editor's pencil is a HOTSPOT the screen's code keeps active: moved off the 1024x768 screen and pointed at
    // this screen, so it can be neither seen nor used.
    if (section_range(t, L"MissionEditorButton", b, e)) {
        set_key(t, b, e, L"Active", L"FALSE");
        set_key(t, b, e, L"Enabled", L"FALSE");
        set_key(t, b, e, L"Position", L"4000,4000");
        set_key(t, b, e, L"DestinationMenu", L"SingleMissionScreen");
    }
    text = std::move(t);
    return true;
}

namespace {
int add_return_item(std::wstring& t, const wchar_t* screen, bool host);  // below
}  // namespace

int splice_debrief_for_lobby(std::wstring& text, bool host) {
    std::wstring t = text;
    size_t b = 0, e = 0;
    if (!section_range(t, L"ExitButton", b, e) || !section_range(t, L"ReflyButton", b, e)) return 0;  // Refly must hide
    const int id = add_return_item(t, L"DebriefScreen", host);
    if (!id || !section_range(t, L"ExitButton", b, e)) return 0;
    set_key(t, b, e, L"DestinationMenu", host ? kLobbyHostScreen : kLobbyJoinScreen);
    section_range(t, L"ReflyButton", b, e);
    set_key(t, b, e, L"Active", L"FALSE");
    set_key(t, b, e, L"Enabled", L"FALSE");
    set_key(t, b, e, L"TextPosition", L"4000,4000");
    if (section_range(t, L"MissionEditorButton", b, e)) {
        set_key(t, b, e, L"Active", L"FALSE");
        set_key(t, b, e, L"Enabled", L"FALSE");
        set_key(t, b, e, L"Position", L"4000,4000");
        set_key(t, b, e, L"DestinationMenu", L"DebriefScreen");
    }
    text = std::move(t);
    return id;
}

int screen_item_number(const std::wstring& text, const wchar_t* screen, const wchar_t* item) {
    size_t b = 0, e = 0;
    if (!section_range(text, screen, b, e)) return 0;
    const std::wstring want = lower(item);
    for (size_t p = text.find(L"MenuItem", b); p != std::wstring::npos && p < e; p = text.find(L"MenuItem", p + 8)) {
        if (p > 0 && text[p - 1] != L'\n') continue;
        int id = 0;
        if (swscanf_s(text.c_str() + p, L"MenuItem%d=", &id) != 1) continue;
        const size_t v = text.find(L'=', p) + 1;
        size_t eol = text.find_first_of(L"\r\n", v);
        if (eol == std::wstring::npos) eol = text.size();
        if (lower(text.substr(v, eol - v)) == want) return id;
    }
    return 0;
}

namespace {

// Adds the hidden [LazyLobbyReturn] TEXTBUTTON (to the host or join screen) as the screen's next MenuItem; returns
// its number, or 0 (t untouched) if the screen section is not as expected.
int add_return_item(std::wstring& t, const wchar_t* screen, bool host) {
    size_t b = 0, e = 0;
    if (!section_range(t, screen, b, e) || lower(t).find(L"[lazylobbyreturn]") != std::wstring::npos) return 0;
    // The next free MenuItem number of the screen, and the line after its last MenuItem line.
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
    if (max_id <= 0 || max_id > 998) return 0;
    const int id = max_id + 1;
    wchar_t line[64];
    swprintf_s(line, L"MenuItem%03d=LazyLobbyReturn\r\n", id);
    t.insert(ins, line);
    if (!t.empty() && t.back() != L'\n') t += L"\r\n";
    t += L"\r\n[LazyLobbyReturn]\r\n"
         L"MenuItemType=TEXTBUTTON\r\n"
         L"DestinationMenu=";
    t += host ? kLobbyHostScreen : kLobbyJoinScreen;
    t += L"\r\n"
         L"TextSetting=LargerTextLeft\r\n"
         L"ButtonSetting=MenuButtonSetting\r\n"
         L"TextStringID=TXT_LAZY_LOBBY_BLANK\r\n"
         L"TextPosition=-200,-200\r\n"
         L"Active=FALSE\r\n"
         L"Enabled=FALSE\r\n";
    return id;
}

}  // namespace

int splice_hangar_for_lobby(std::wstring& text, bool host) {
    std::wstring t = text;
    size_t b = 0, e = 0;
    const int id = add_return_item(t, L"HangarScreen", host);
    if (!id) return 0;
    if (section_range(t, L"FlyButton", b, e)) {
        set_key(t, b, e, L"TextStringID", host ? L"TXT_LAZY_LOBBY_DONE" : L"TXT_LAZY_LOBBY_READY");
        set_key(t, b, e, L"InfoTextStringID", host ? L"TXT_LAZY_LOBBY_DONE_INFO" : L"TXT_LAZY_LOBBY_HANGAR_READY");
    }
    if (!host && section_range(t, L"LoadoutButton", b, e)) {  // clients fly the host's loadout (v1)
        set_key(t, b, e, L"Active", L"FALSE");
        set_key(t, b, e, L"Enabled", L"FALSE");
    }
    text = std::move(t);
    return id;
}

bool splice_lobby_screen(std::wstring& text, const std::wstring& menu_code_dll) {
    const std::wstring low = lower(text);
    for (const wchar_t* s : {kLobbyScreen, kLobbyHostScreen, kLobbyJoinScreen}) {
        if (low.find(L"[" + lower(s) + L"]") != std::wstring::npos) return false;
    }
    // The ini value runs to the end of the line, and MenuSystem treats a name with ':' as an absolute path.
    if (menu_code_dll.size() < 4 || menu_code_dll.size() > 200 || menu_code_dll[1] != L':' ||
        menu_code_dll.find_first_of(L"\r\n;[]") != std::wstring::npos) {
        return false;
    }

    Ini ini;
    ini.t = text;
    if (!ini.t.empty() && ini.t.back() != L'\n') ini.t += L"\r\n";
    ini.t += L"\r\n";
    // Connect screen on the Hangar background: its left panel (x 20-490, header rule at y 146) holds the form; Back
    // where the stock screens put Exit.
    ini.screen(kLobbyScreen, L"DefaultHangarScreen.JPG",
               {L"LazyLobbyBackButton", L"LazyLobbyHostButton", L"LazyLobbyJoinButton", L"LazyLobbyTitle",
                L"LazyLobbyPanelTitle", L"LazyLobbyCallsignLabel", L"LazyLobbyCallsign", L"LazyLobbyAddressLabel",
                L"LazyLobbyAddress", L"LazyLobbyAddressHint", L"LazyLobbyStatus", L"LazyLobbyNetHint1",
                L"LazyLobbyNetHint2", L"LazyLobbyPasswordLabel", L"LazyLobbyPassword", L"LazyLobbyPasswordHint"},
               menu_code_dll);
    ini.button(L"LazyLobbyBackButton", L"MainScreen", L"LargerTextLeft", L"TXT_MENU_BACK", 48, 694,
               L"TXT_MENU_BACK_MAIN_INFO");
    ini.button(L"LazyLobbyHostButton", kLobbyHostScreen, L"LargerTextCenter", L"TXT_MENU_HOST", 112, 350,
               L"TXT_MENU_HOST_INFO");
    ini.button(L"LazyLobbyJoinButton", kLobbyJoinScreen, L"LargerTextCenter", L"TXT_MENU_JOIN", 257, 350,
               L"TXT_LAZY_LOBBY_JOIN_INFO");
    ini.text(L"LazyLobbyTitle", L"LazyLobbyTitleText", L"TXT_MENU_MULTIPLAYER_TITLE", -1, -1);
    ini.text(L"LazyLobbyPanelTitle", L"LargerTextCenter", L"TXT_MENU_TYPE_MISSION", 257, 120);
    ini.text(L"LazyLobbyCallsignLabel", L"MediumTextLeft", L"TXT_MENU_CALLSIGNTEXT", 46, 172);
    ini.text(L"LazyLobbyCallsign", L"MediumTextLeft", kLobbyBlankKey, 180, 172);
    ini.text(L"LazyLobbyAddressLabel", L"MediumTextLeft", L"TXT_MENU_ADDRESS_TEXT", 46, 209);
    ini.edit(L"LazyLobbyAddress", 180, 206, kLobbyAddressChars, L"TXT_MENU_TCPIP_INFO");
    ini.text(L"LazyLobbyAddressHint", L"MediumTextLeft", L"TXT_MENU_TCPIP_INFO", 46, 240);
    ini.text(L"LazyLobbyStatus", L"MediumTextLeft", L"TXT_LAZY_LOBBY_STATUS", 46, 400);
    ini.text(L"LazyLobbyNetHint1", L"MediumTextLeft", L"TXT_LAZY_LOBBY_NET_HINT1", 46, 432);
    ini.text(L"LazyLobbyNetHint2", L"MediumTextLeft", L"TXT_LAZY_LOBBY_NET_HINT2", 46, 452);
    // Under the Address and its hint: the label, the field and its hint line (the field's info text).
    ini.text(L"LazyLobbyPasswordLabel", L"MediumTextLeft", L"TXT_LAZY_LOBBY_PASSWORD", 46, 275);
    ini.edit(L"LazyLobbyPassword", 180, 272, kLobbyPasswordChars, L"TXT_LAZY_LOBBY_PASS_HINT");
    ini.text(L"LazyLobbyPasswordHint", L"MediumTextLeft", L"TXT_LAZY_LOBBY_PASS_HINT", 46, 304);

    session_screen(ini, true, menu_code_dll);
    session_screen(ini, false, menu_code_dll);

    // The stock screen title style lives in TitleTextSetting.ini, which MenuSystem.ini does not include.
    ini.t += L"[LazyLobbyTitleText]\r\n"
             L"TextFontName=Impact\r\n"
             L"TextSize=32\r\n"
             L"TextBold=FALSE\r\n"
             L"TextItalic=FALSE\r\n"
             L"TextUnderline=FALSE\r\n"
             L"TextStrikeOut=FALSE\r\n"
             L"TextShadow=FALSE\r\n"
             L"HorizontalAlignment=RIGHT\r\n"
             L"VerticalAlignment=CENTER\r\n"
             L"ForegroundColor=250,250,250\r\n"
             L"TransparentBackground=FALSE\r\n"
             L"BackgroundColor=0,0,0\r\n"
             L"Position=999,25\r\n";
    // The seat rows' colours (MediumTextLeft otherwise): your seat green, another pilot's blue, a free one yellow, all
    // light enough for the dark panel.
    for (const auto& [name, colour] : {std::pair{L"LazyRowOwnText", L"110,230,110"}, std::pair{L"LazyRowOtherText", L"130,180,255"},
                                       std::pair{L"LazyRowOpenText", L"235,215,90"}}) {
        ini.t += L"[" + std::wstring(name) + L"]\r\n"
                 L"TextFontName=Arial\r\n"
                 L"TextSize=16\r\n"
                 L"TextBold=FALSE\r\n"
                 L"TextItalic=FALSE\r\n"
                 L"TextUnderline=FALSE\r\n"
                 L"TextStrikeOut=FALSE\r\n"
                 L"TextShadow=FALSE\r\n"
                 L"HorizontalAlignment=LEFT\r\n"
                 L"VerticalAlignment=TOP\r\n"
                 L"ForegroundColor=" + std::wstring(colour) + L"\r\n\r\n";
    }
    text = std::move(ini.t);
    return true;
}

}  // namespace LazyWorks::SF2Patch::hooks
