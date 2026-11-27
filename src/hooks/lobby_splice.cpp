#include "hooks/lobby_splice.h"

#include <cwchar>
#include <cwctype>

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

}  // namespace

bool splice_main_screen_button(std::wstring& text) {
    std::wstring t = text;
    size_t b = 0, e = 0;
    if (!section_range(t, L"MultiplayerHotspot", b, e)) return false;
    if (lower(t.substr(b, e - b)).find(L"destinationmenu=" + lower(kLobbyScreen)) == std::wstring::npos) return false;
    set_key(t, b, e, L"Active", L"TRUE");
    set_key(t, b, e, L"Enabled", L"TRUE");
    // Stock: the 48-point style at (790,438) overlaps Campaign; Pilot Record's 40-point style fits the gap.
    set_key(t, b, e, L"TextSetting", L"MainMenuText2");
    set_key(t, b, e, L"TextPosition", L"790,459");
    text = std::move(t);
    return true;
}

bool splice_lobby_screen(std::wstring& text, const std::wstring& menu_code_dll) {
    if (lower(text).find(L"[" + lower(kLobbyScreen) + L"]") != std::wstring::npos) return false;
    // The ini value runs to the end of the line, and MenuSystem treats a name with ':' as an absolute path.
    if (menu_code_dll.size() < 4 || menu_code_dll.size() > 200 || menu_code_dll[1] != L':' ||
        menu_code_dll.find_first_of(L"\r\n;[]") != std::wstring::npos) {
        return false;
    }

    std::wstring t = text;
    if (!t.empty() && t.back() != L'\n') t += L"\r\n";
    // Laid out on the Hangar background: the left panel (x 20-490, header rule at y 146), buttons in the panel's
    // bottom row like the Hangar's, Back where the stock screens put Exit.
    t += L"\r\n[NetworkConnectScreen]\r\n"
         L"MenuType=NORMAL_MENU\r\n"
         L"BackgroundImage=DefaultHangarScreen.JPG\r\n"
         L"MenuItem001=LazyLobbyBackButton\r\n"
         L"MenuItem002=LazyLobbyHostButton\r\n"
         L"MenuItem003=LazyLobbyJoinButton\r\n"
         L"MenuItem004=LazyLobbyTitle\r\n"
         L"MenuItem005=LazyLobbyPanelTitle\r\n"
         L"MenuItem006=LazyLobbyCallsignLabel\r\n"
         L"MenuItem007=LazyLobbyCallsign\r\n"
         L"MenuItem008=LazyLobbyAddressLabel\r\n"
         L"MenuItem009=LazyLobbyAddress\r\n"
         L"MenuItem010=LazyLobbyStatus\r\n"
         L"MenuItem011=LazyLobbyPlayersLabel\r\n"
         L"MenuItem012=LazyLobbyPlayers\r\n"
         L"MenuCodeDLL=";
    t += menu_code_dll;
    t += L"\r\n"
         L"RememberMenu=TRUE\r\n"
         L"DefaultSelection=-1\r\n"
         L"MouseAnimation=MouseAnimation\r\n"
         L"BackgroundMusic=\r\n"
         L"TextFilename=NetworkConnectScreen.str\r\n"
         L"\r\n"
         L"[LazyLobbyBackButton]\r\n"
         L"MenuItemType=TEXTBUTTON\r\n"
         L"DestinationMenu=MainScreen\r\n"
         L"TextSetting=LargerTextLeft\r\n"
         L"ButtonSetting=MenuButtonSetting\r\n"
         L"TextStringID=TXT_MENU_BACK\r\n"
         L"TextPosition=48,694\r\n"
         L"InfoTextSetting=InfoTextSetting\r\n"
         L"InfoTextStringID=TXT_MENU_BACK_MAIN_INFO\r\n"
         L"\r\n"
         L"[LazyLobbyHostButton]\r\n"
         L"MenuItemType=TEXTBUTTON\r\n"
         L"DestinationMenu=\r\n"
         L"TextSetting=LargerTextCenter\r\n"
         L"ButtonSetting=MenuButtonSetting\r\n"
         L"TextStringID=TXT_MENU_HOST\r\n"
         L"TextPosition=112,614\r\n"
         L"InfoTextSetting=InfoTextSetting\r\n"
         L"InfoTextStringID=TXT_MENU_HOST_INFO\r\n"
         L"\r\n"
         L"[LazyLobbyJoinButton]\r\n"
         L"MenuItemType=TEXTBUTTON\r\n"
         L"DestinationMenu=\r\n"
         L"TextSetting=LargerTextCenter\r\n"
         L"ButtonSetting=MenuButtonSetting\r\n"
         L"TextStringID=TXT_MENU_JOIN\r\n"
         L"TextPosition=257,614\r\n"
         L"InfoTextSetting=InfoTextSetting\r\n"
         L"InfoTextStringID=";
    t += kLobbyJoinInfoKey;
    t += L"\r\n"
         L"\r\n"
         L"[LazyLobbyTitle]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=LazyLobbyTitleText\r\n"
         L"TextStringID=TXT_MENU_MULTIPLAYER_TITLE\r\n"
         L"\r\n"
         L"[LazyLobbyPanelTitle]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=LargerTextCenter\r\n"
         L"TextStringID=TXT_MENU_TYPE_MISSION\r\n"
         L"TextPosition=257,120\r\n"
         L"\r\n"
         L"[LazyLobbyCallsignLabel]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=MediumTextLeft\r\n"
         L"TextStringID=TXT_MENU_CALLSIGNTEXT\r\n"
         L"TextPosition=46,172\r\n"
         L"\r\n"
         L"[LazyLobbyCallsign]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=MediumTextLeft\r\n"
         L"TextStringID=\r\n"
         L"TextPosition=180,172\r\n"
         L"\r\n"
         L"[LazyLobbyAddressLabel]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=MediumTextLeft\r\n"
         L"TextStringID=TXT_MENU_ADDRESS_TEXT\r\n"
         L"TextPosition=46,209\r\n"
         L"\r\n"
         L"[LazyLobbyAddress]\r\n"
         L"MenuItemType=EDIT\r\n"
         L"Position=180,206\r\n"
         L"Width=280\r\n"
         L"Height=22\r\n"
         L"EditFieldSetting=EditFieldSetting\r\n"
         L"TextBufferSize=64\r\n"
         L"NumberOnly=FALSE\r\n"
         L"TabSelectable=TRUE\r\n"
         L"DefaultTextStringID=\r\n"
         L"InfoTextSetting=InfoTextSetting\r\n"
         L"InfoTextStringID=TXT_MENU_TCPIP_INFO\r\n"
         L"\r\n"
         L"[LazyLobbyStatus]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=MediumTextLeft\r\n"
         L"TextStringID=";
    t += kLobbyStatusKey;
    t += L"\r\n"
         L"TextPosition=46,246\r\n"
         L"\r\n"
         L"[LazyLobbyPlayersLabel]\r\n"
         L"MenuItemType=TEXT\r\n"
         L"TextSetting=MediumTextLeft\r\n"
         L"TextStringID=TXT_MENU_NUM_PLAYERS\r\n"
         L"TextPosition=46,290\r\n"
         L"\r\n"
         L"[LazyLobbyPlayers]\r\n"
         L"MenuItemType=TEXTAREA\r\n"
         L"TextSetting=MediumTextLeft\r\n"
         L"ScrollBarSetting=ScrollBarSetting\r\n"
         L"Position=46,312\r\n"
         L"Width=430\r\n"
         L"Height=270\r\n"
         L"Spacing=2\r\n"
         L"\r\n"
         // The stock screen title style lives in TitleTextSetting.ini, which MenuSystem.ini does not include.
         L"[LazyLobbyTitleText]\r\n"
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
    text = std::move(t);
    return true;
}

}  // namespace LazyWorks::SF2Patch::hooks
