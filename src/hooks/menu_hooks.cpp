// Options menu rows the mod adds: Display Mode and Screen (Graphics), Mouse Pointer and Pilot Twist (Controls), Tacview Recording
// (Gameplay). Splices menu text as the file service reads it; hooks the OptionsScreen sub-menu classes at fixed RVAs.
// See lazy-sf2-recon: option-trace.md.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <string>
#include <vector>

#include "config.h"
#include "monitors.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/options_gameplay_splice.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

// Build stamps and RVAs.
constexpr DWORD kExeTimestamp = 0x51d22110, kExeImageSize = 0x62000;
constexpr DWORD kFileOpenRva = 0x94e0;   // file service Open
constexpr DWORD kFileCloseRva = 0x9590;  // file service Close
constexpr DWORD kFileReadRva = 0x8270;   // file service Read

constexpr DWORD kOptsTimestamp = 0x51d220d6, kOptsImageSize = 0x2c000;
constexpr DWORD kGfxInitRva = 0x58f0;     // Graphics page Init
constexpr DWORD kGfxRefreshRva = 0x5dd0;  // Graphics page Refresh
constexpr DWORD kGfxChangedRva = 0x4e20;  // Graphics page Changed
constexpr DWORD kGfxExitRva = 0x5da0;     // Graphics page Exit
// Controls sub-menu class (vtable +0x1e9a0).
constexpr DWORD kCtlInitRva = 0x3500;     // Controls page Init
constexpr DWORD kCtlExitRva = 0x1f40;     // Controls page Exit
constexpr DWORD kCtlRefreshRva = 0x1fa0;  // Controls page Refresh
constexpr DWORD kCtlChangedRva = 0x2010;  // Controls page Changed
// Gameplay sub-menu class (vtable +0x1e934). Its methods use fixed control IDs 1..15 and 23..32, so our higher IDs
// are left alone.
constexpr DWORD kGplInitRva = 0x4540;     // Gameplay page Init
constexpr DWORD kGplExitRva = 0x4220;     // Gameplay page Exit (saves [GameplayOptions])
constexpr DWORD kGplRefreshRva = 0x4230;  // Gameplay page Refresh
constexpr DWORD kGplChangedRva = 0x3ab0;  // Gameplay page Changed
constexpr DWORD kIniReadIntRva = 0xa790;  // Options.ini ReadInt
constexpr DWORD kIniWriteIntRva = 0xa0e0; // Options.ini WriteInt
constexpr DWORD kIniGlobalRva = 0x26a90;  // Options.ini object used by the screen

// Menu and control interfaces (vtable byte offsets / 4).
constexpr int kMenuGetItem = 0x28 / 4;   // int GetItem(int id, int type, Control** out)
constexpr int kListAddItem = 0x20 / 4;   // void AddItem(const wchar_t* text, int value)
constexpr int kListSetSel = 0x30 / 4;    // void SetSelection(int index)
constexpr int kListGetSel = 0x34 / 4;    // int GetSelection()  (-1 = none)
constexpr int kItemTypeList = 8;

constexpr wchar_t kSection[] = L"GraphicsOptions";
constexpr wchar_t kKey[] = L"DisplayMode";

// Text service (Core\TextUSEnglish.dll, July 2013).
constexpr DWORD kTextTimestamp = 0x51d21fb5, kTextImageSize = 0x1e000;
constexpr DWORD kGetTextRva = 0x1f50;   // const wchar_t* GetText(file, key)   (returns key if unknown)
constexpr DWORD kCopyTextRva = 0x1ed0;  // int CopyText(file, key, wchar_t* buf, size_t n) (1 = found)
constexpr wchar_t kHintKey[] = L"TXT_LAZY_DISPLAYMODE_INFO";
// One line, at most 80 characters (the Graphics page cuts off longer hints).
constexpr wchar_t kHintText[] = L"Select fullscreen, or a borderless window for instant Alt-Tab.";

std::atomic<int> g_list_id{0};       // control ID assigned to our list (0 = not injected)
std::atomic<int> g_mouse_list_id{0}; // Controls page: Mouse Pointer list ID (0 = not injected)
std::atomic<int> g_tac_list_id{0};   // Gameplay page: Tacview Recording list ID (0 = not injected)
int g_selected = 0;                  // current selection on the Graphics page (UI thread)
constexpr int kMaxTracked = 8;
std::atomic<int> g_tracked[kMaxTracked] = {};  // open handles of OptionsScreen.ini (+1, 0 = free)

bool ends_with_options_screen_ini(const wchar_t* name) {
    if (!name) return false;
    constexpr wchar_t kName[] = L"OptionsScreen.ini";
    const size_t n = wcslen(name), k = wcslen(kName);
    if (n < k || _wcsicmp(name + n - k, kName) != 0) return false;
    return n == k || name[n - k - 1] == L'\\' || name[n - k - 1] == L'/';
}

void track(int handle) {
    for (auto& t : g_tracked) {
        int expected = 0;
        if (t.compare_exchange_strong(expected, handle + 1)) return;
    }
}

bool untrack(int handle) {
    for (auto& t : g_tracked) {
        int expected = handle + 1;
        if (t.compare_exchange_strong(expected, 0)) return true;
    }
    return false;
}

// Our row goes where Detail is now (directly below Aspect Ratio); the top block uses 40-unit rows.
constexpr int kInsertY = 120;
constexpr int kRowStep = 80;  // two new rows (Display Mode, Screen) of 40 each

// Names listed as MenuItemNNN=<name> in the section body [body, end).
std::vector<std::wstring> item_names(const std::wstring& text, size_t body, size_t end) {
    std::vector<std::wstring> names;
    for (size_t p = body; p < end;) {
        size_t eol = text.find(L'\n', p);
        if (eol == std::wstring::npos || eol > end) eol = end;
        std::wstring line = text.substr(p, eol - p);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(L"MenuItem", 0) == 0 && line.find(L"MenuItemType") != 0) {
            if (size_t eq = line.find(L'='); eq != std::wstring::npos) names.push_back(line.substr(eq + 1));
        }
        p = eol + 1;
    }
    return names;
}

// Moves every listed item whose Position/TextPosition y is at or below kInsertY down by kRowStep.
void shift_rows(std::wstring& text, const std::vector<std::wstring>& names) {
    int moved = 0;
    for (const std::wstring& name : names) {
        size_t sec = text.find(L"[" + name + L"]");
        if (sec == std::wstring::npos) continue;
        size_t p = text.find(L'\n', sec);
        while (p != std::wstring::npos && p + 1 < text.size() && text[p + 1] != L'[') {
            const size_t line = p + 1;
            size_t eol = text.find(L'\n', line);
            if (eol == std::wstring::npos) eol = text.size();
            for (const wchar_t* key : {L"Position=", L"TextPosition="}) {
                const size_t klen = wcslen(key);
                if (text.compare(line, klen, key) != 0) continue;
                int x = 0, y = 0;
                if (swscanf_s(text.c_str() + line + klen, L"%d,%d", &x, &y) != 2 || y < kInsertY) break;
                size_t vend = text.find_first_of(L"\r\n", line + klen);
                if (vend == std::wstring::npos) vend = text.size();
                text.replace(line + klen, vend - (line + klen), std::to_wstring(x) + L"," + std::to_wstring(y + kRowStep));
                eol = text.find(L'\n', line);
                if (eol == std::wstring::npos) eol = text.size();
                ++moved;
                break;
            }
            p = eol;
        }
    }
    LOG("menu: moved %d Graphics page positions down one row", moved);
}

// Controls page: "Mouse Pointer" and "Pilot Twist" rows in the free slots below Keyboard Map (y=320), styled like it.
// Nothing moves. Returns the Mouse Pointer list's control ID (Pilot Twist's list is that + 2), or 0 if the page isn't
// as expected.
int splice_controls(std::wstring& text) {
    constexpr wchar_t kHeader[] = L"[ControlOptionsSubMenu]";
    size_t sec = text.find(kHeader);
    if (sec == std::wstring::npos || text.find(L"[MouseModeList]") != std::wstring::npos) return 0;
    size_t body = text.find(L'\n', sec);
    if (body == std::wstring::npos) return 0;
    ++body;
    size_t end = text.find(L"\n[", body);
    end = (end == std::wstring::npos) ? text.size() : end + 1;

    int max_id = 0;
    for (size_t p = text.find(L"MenuItem", body); p != std::wstring::npos && p < end; p = text.find(L"MenuItem", p + 8)) {
        if (p > 0 && text[p - 1] != L'\n') continue;
        int id = 0;
        if (swscanf_s(text.c_str() + p, L"MenuItem%d=", &id) == 1 && id > max_id) max_id = id;
    }
    if (max_id <= 0 || max_id > 997) return 0;
    const int id = max_id + 1;

    wchar_t items[192];
    swprintf_s(items,
               L"MenuItem%03d=MouseModeList\r\nMenuItem%03d=MouseModeText\r\n"
               L"MenuItem%03d=PilotTwistList\r\nMenuItem%03d=PilotTwistText\r\n",
               id, id + 1, id + 2, id + 3);
    text.insert(end, items);
    if (!text.empty() && text.back() != L'\n') text += L"\r\n";
    text +=
        L"\r\n"
        L"[MouseModeList]\r\n"
        L"MenuItemType=LIST\r\n"
        L"Position=200,360\r\n"
        L"Width=240\r\n"
        L"Height=22\r\n"
        L"ListFieldSetting=MediumListField\r\n"
        L"DropDownListSize=10\r\n"
        L"InfoTextSetting=InfoTextSetting\r\n"
        L"InfoTextStringID=TXT_LAZY_MOUSE_INFO\r\n"
        L"\r\n"
        L"[MouseModeText]\r\n"
        L"MenuItemType=TEXT\r\n"
        L"TextSetting=MediumTextLeft\r\n"
        L"TextStringID=Mouse Pointer\r\n"
        L"TextPosition=0,363\r\n"
        L"\r\n"
        L"[PilotTwistList]\r\n"
        L"MenuItemType=LIST\r\n"
        L"Position=200,400\r\n"
        L"Width=240\r\n"
        L"Height=22\r\n"
        L"ListFieldSetting=MediumListField\r\n"
        L"DropDownListSize=2\r\n"
        L"InfoTextSetting=InfoTextSetting\r\n"
        L"InfoTextStringID=TXT_LAZY_PILOT_TWIST_INFO\r\n"
        L"\r\n"
        L"[PilotTwistText]\r\n"
        L"MenuItemType=TEXT\r\n"
        L"TextSetting=MediumTextLeft\r\n"
        L"TextStringID=Pilot Twist\r\n"
        L"TextPosition=0,403\r\n";
    return id;
}

// Returns the spliced text, or an empty string if the text is not what we expect.
std::wstring splice(const std::wstring& text, int& list_id) {
    if (text.find(L"[DisplayModeList]") != std::wstring::npos) {
        LOG("menu: OptionsScreen.ini already defines [DisplayModeList]; leaving it unchanged");
        return {};
    }
    constexpr wchar_t kHeader[] = L"[GraphicsOptionsSubMenu]";
    size_t sec = text.find(kHeader);
    if (sec == std::wstring::npos) return {};
    size_t body = text.find(L'\n', sec);
    if (body == std::wstring::npos) return {};
    ++body;
    size_t end = text.find(L"\n[", body);  // start of the next section's line
    end = (end == std::wstring::npos) ? text.size() : end + 1;

    // Highest MenuItemNNN in the section.
    int max_id = 0;
    for (size_t p = text.find(L"MenuItem", body); p != std::wstring::npos && p < end; p = text.find(L"MenuItem", p + 8)) {
        if (p > 0 && text[p - 1] != L'\n') continue;
        int id = 0;
        if (swscanf_s(text.c_str() + p, L"MenuItem%d=", &id) == 1 && id > max_id) max_id = id;
    }
    if (max_id <= 0 || max_id > 997) return {};
    list_id = max_id + 1;

    wchar_t items[256];
    swprintf_s(items,
               L"MenuItem%03d=DisplayModeList\r\nMenuItem%03d=DisplayModeText\r\n"
               L"MenuItem%03d=ScreenList\r\nMenuItem%03d=ScreenText\r\n",
               list_id, list_id + 1, list_id + 2, list_id + 3);

    // Two new rows in the top block, directly below Aspect Ratio (y=80), styled like Aspect Ratio.
    // Everything on the Graphics page at or below them moves down two rows (see shift_rows).
    constexpr wchar_t kSections[] =
        L"\r\n"
        L"[DisplayModeList]\r\n"
        L"MenuItemType=LIST\r\n"
        L"Position=200,120\r\n"
        L"Width=240\r\n"
        L"Height=22\r\n"
        L"ListFieldSetting=MediumListField\r\n"
        L"DropDownListSize=10\r\n"
        L"InfoTextSetting=InfoTextSetting\r\n"
        // String IDs are cut to ~31 characters, so the hint uses a short key that our text hook
        // answers (see kHintKey); the label is short enough to be its own literal text.
        L"InfoTextStringID=TXT_LAZY_DISPLAYMODE_INFO\r\n"
        L"\r\n"
        L"[DisplayModeText]\r\n"
        L"MenuItemType=TEXT\r\n"
        L"TextSetting=MediumTextLeft\r\n"
        L"TextStringID=Display Mode\r\n"
        L"TextPosition=0,123\r\n"
        L"\r\n"
        L"[ScreenList]\r\n"
        L"MenuItemType=LIST\r\n"
        L"Position=200,160\r\n"
        L"Width=240\r\n"
        L"Height=22\r\n"
        L"ListFieldSetting=MediumListField\r\n"
        L"DropDownListSize=10\r\n"
        L"InfoTextSetting=InfoTextSetting\r\n"
        L"InfoTextStringID=TXT_LAZY_SCREEN_INFO\r\n"
        L"\r\n"
        L"[ScreenText]\r\n"
        L"MenuItemType=TEXT\r\n"
        L"TextSetting=MediumTextLeft\r\n"
        L"TextStringID=Screen\r\n"
        L"TextPosition=0,163\r\n";

    std::wstring out = text.substr(0, end);
    if (!out.empty() && out.back() != L'\n') out += L"\r\n";
    out += items;
    out += text.substr(end);
    shift_rows(out, item_names(out, body, end));
    if (!out.empty() && out.back() != L'\n') out += L"\r\n";
    out += kSections;
    return out;
}

using FileOpen_t = int(__fastcall*)(void*, void*, const wchar_t*, int);
using FileClose_t = void(__fastcall*)(void*, void*, int);
using FileRead_t = int(__fastcall*)(void*, void*, int, char*, unsigned);
FileOpen_t o_FileOpen = nullptr;
FileClose_t o_FileClose = nullptr;
FileRead_t o_FileRead = nullptr;

int __fastcall h_FileOpen(void* svc, void* edx, const wchar_t* name, int mode) {
    int h = o_FileOpen(svc, edx, name, mode);
    if (h != -1 && ends_with_options_screen_ini(name)) {
        LOG("menu: opened %ls (mode %d) as handle %d", name, mode, h);
        track(h);
    }
    return h;
}

void __fastcall h_FileClose(void* svc, void* edx, int handle) {
    untrack(handle);
    o_FileClose(svc, edx, handle);
}

int __fastcall h_FileRead(void* svc, void* edx, int handle, char* buf, unsigned bytes) {
    int n = o_FileRead(svc, edx, handle, buf, bytes);
    if (!untrack(handle)) return n;  // not ours, or already spliced
    if (n <= 0 || static_cast<unsigned>(n) >= bytes) {
        LOG("menu: OptionsScreen.ini read of %d/%u bytes is not a whole-file read; not modified", n, bytes);
        return n;
    }

    // UTF-16 LE with BOM (stock), or 8-bit text.
    const bool wide = n >= 2 && static_cast<unsigned char>(buf[0]) == 0xFF && static_cast<unsigned char>(buf[1]) == 0xFE;
    std::wstring text;
    if (wide) {
        text.assign(reinterpret_cast<const wchar_t*>(buf + 2), (n - 2) / 2);
    } else {
        for (int i = 0; i < n; ++i) text += static_cast<wchar_t>(static_cast<unsigned char>(buf[i]));
    }

    int list_id = 0;
    std::wstring out = splice(text, list_id);
    if (out.empty()) {
        LOG("menu: OptionsScreen.ini has no usable [GraphicsOptionsSubMenu]; Display Mode not added");
        return n;
    }
    const int mouse_id = splice_controls(out);
    const int tac_id = splice_gameplay_row(out);
    const size_t need = wide ? 2 + out.size() * 2 : out.size();
    if (need + 2 > bytes) {  // keep room for a 2-byte terminator
        LOG("menu: no room to add Display Mode (%zu of %u bytes)", need, bytes);
        return n;
    }
    if (wide) {
        memcpy(buf + 2, out.data(), out.size() * 2);
    } else {
        for (size_t i = 0; i < out.size(); ++i) buf[i] = static_cast<char>(out[i]);
    }
    g_list_id = list_id;
    g_mouse_list_id = mouse_id;
    g_tac_list_id = tac_id;
    LOG("menu: added Display Mode as MenuItem%03d, Mouse Pointer and Pilot Twist as Controls MenuItem%03d and %03d, "
        "Tacview Recording as Gameplay MenuItem%03d (%d -> %zu bytes)",
        list_id, mouse_id, mouse_id ? mouse_id + 2 : 0, tac_id, n, need);
    return static_cast<int>(need);
}

HMODULE g_opts = nullptr;
using Method_t = void(__fastcall*)(void* self, void* edx);
using Init_t = void(__fastcall*)(void* self, void* edx, int* a, int* b);
using IniReadInt_t = int(__fastcall*)(void* ini, void* edx, const wchar_t* section, const wchar_t* key, int def);
using IniWriteInt_t = void(__fastcall*)(void* ini, void* edx, const wchar_t* section, const wchar_t* key, int value);
Init_t o_Init = nullptr;
Method_t o_Refresh = nullptr;
Method_t o_Changed = nullptr;
Method_t o_Exit = nullptr;

void* options_ini() { return *static_cast<void**>(at_rva(g_opts, kIniGlobalRva)); }

// Screen list: one entry per monitor, as "Screen N (WxH)". Labels must outlive the list control,
// so they are kept here and rebuilt each time the page opens.
std::vector<int> g_screen_numbers;      // list index -> \\.\DISPLAYn number
std::vector<std::wstring> g_screen_labels;
int g_screen_selected = 0;              // list index

// A list control on the current Graphics page (offset 0 = Display Mode, 2 = Screen), or nullptr.
void* our_list(void* self, int offset = 0) {
    const int id = g_list_id.load();
    void* menu = *reinterpret_cast<void**>(static_cast<BYTE*>(self) + 4);
    if (!id || !menu) return nullptr;
    void* ctrl = nullptr;
    using GetItem_t = int(__fastcall*)(void*, void*, int, int, void**);
    reinterpret_cast<GetItem_t>(vtable_slot(menu, kMenuGetItem))(menu, nullptr, id + offset, kItemTypeList, &ctrl);
    return ctrl;
}
constexpr int kScreenOffset = 2;

using AddItem_t = void(__fastcall*)(void*, void*, const wchar_t*, int);
using SetSel_t = void(__fastcall*)(void*, void*, int);
using GetSel_t = int(__fastcall*)(void*, void*);

void fill_screen_list(void* list) {
    g_screen_numbers.clear();
    g_screen_labels.clear();
    for (const monitors::Monitor& m : monitors::list()) {
        wchar_t label[64];
        swprintf_s(label, L"Screen %d (%ldx%ld)", m.number, m.rect.right - m.rect.left, m.rect.bottom - m.rect.top);
        g_screen_numbers.push_back(m.number);
        g_screen_labels.emplace_back(label);
    }
    auto add = reinterpret_cast<AddItem_t>(vtable_slot(list, kListAddItem));
    for (size_t i = 0; i < g_screen_labels.size(); ++i) add(list, nullptr, g_screen_labels[i].c_str(), static_cast<int>(i));

    // Select the configured Screen, else the one the game is on now.
    int want = config::display_monitor();
    if (want <= 0 || std::find(g_screen_numbers.begin(), g_screen_numbers.end(), want) == g_screen_numbers.end()) {
        want = monitors::number_of_window(game_window());
    }
    auto it = std::find(g_screen_numbers.begin(), g_screen_numbers.end(), want);
    g_screen_selected = it == g_screen_numbers.end() ? 0 : static_cast<int>(it - g_screen_numbers.begin());
}

void __fastcall h_Init(void* self, void* edx, int* a, int* b) {
    o_Init(self, edx, a, b);
    void* list = our_list(self);
    if (!list) {
        if (g_list_id.load()) LOG("menu: Display Mode list control %d not found", g_list_id.load());
        return;
    }
    auto add = reinterpret_cast<AddItem_t>(vtable_slot(list, kListAddItem));
    add(list, nullptr, L"Fullscreen", static_cast<int>(config::DisplayMode::Fullscreen));
    add(list, nullptr, L"Borderless", static_cast<int>(config::DisplayMode::Borderless));

    void* ini = options_ini();
    const int v = ini ? reinterpret_cast<IniReadInt_t>(at_rva(g_opts, kIniReadIntRva))(ini, nullptr, kSection, kKey, 0) : 0;
    g_selected = (v == static_cast<int>(config::DisplayMode::Borderless)) ? v : 0;

    if (void* screens = our_list(self, kScreenOffset)) fill_screen_list(screens);
    LOG("menu: Graphics page opened, DisplayMode=%d, Screen=%d (%zu screens)", g_selected,
        g_screen_numbers.empty() ? 0 : g_screen_numbers[g_screen_selected], g_screen_numbers.size());
}

void __fastcall h_Refresh(void* self, void* edx) {
    o_Refresh(self, edx);
    if (void* list = our_list(self)) reinterpret_cast<SetSel_t>(vtable_slot(list, kListSetSel))(list, nullptr, g_selected);
    if (void* list = our_list(self, kScreenOffset)) {
        reinterpret_cast<SetSel_t>(vtable_slot(list, kListSetSel))(list, nullptr, g_screen_selected);
    }
}

void __fastcall h_Changed(void* self, void* edx) {
    o_Changed(self, edx);
    if (void* list = our_list(self)) {
        const int sel = reinterpret_cast<GetSel_t>(vtable_slot(list, kListGetSel))(list, nullptr);
        if (sel == 0 || sel == 1) g_selected = sel;
    }
    if (void* list = our_list(self, kScreenOffset)) {
        const int sel = reinterpret_cast<GetSel_t>(vtable_slot(list, kListGetSel))(list, nullptr);
        if (sel >= 0 && sel < static_cast<int>(g_screen_numbers.size())) g_screen_selected = sel;
    }
}

// The page's resolution fields, saved by the stock Save as [GraphicsOptions] DisplayWidth/DisplayHeight. The exe
// reads them only at start-up, so a resolution change applies after a restart.
constexpr int kPageDisplayWidth = 0x10;
constexpr int kPageDisplayHeight = 0x14;

void __fastcall h_Exit(void* self, void* edx) {
    // Write before the stock Exit (which saves the stock keys and tears the page down).
    if (g_list_id.load()) {
        const int screen = g_screen_numbers.empty() ? 0 : g_screen_numbers[g_screen_selected];
        // New Screen: match the game resolution to it (sharp picture from the next start; until then
        // DXGI scales the current resolution to the new monitor).
        RECT r;
        if (screen > 0 && screen != monitors::number_of_window(game_window()) && monitors::rect_of(screen, &r)) {
            auto* page = static_cast<BYTE*>(self);
            *reinterpret_cast<int*>(page + kPageDisplayWidth) = r.right - r.left;
            *reinterpret_cast<int*>(page + kPageDisplayHeight) = r.bottom - r.top;
            LOG("menu: game resolution set to %ldx%ld for Screen %d (applies after a restart)", r.right - r.left,
                r.bottom - r.top, screen);
        }
        if (void* ini = options_ini()) {
            auto write = reinterpret_cast<IniWriteInt_t>(at_rva(g_opts, kIniWriteIntRva));
            write(ini, nullptr, kSection, kKey, g_selected);
            if (screen > 0) write(ini, nullptr, kSection, L"DisplayMonitor", screen);
            LOG("menu: saved [GraphicsOptions] DisplayMode=%d DisplayMonitor=%d", g_selected, screen);
        }
        const auto chosen = static_cast<config::DisplayMode>(g_selected);
        bool changed = false;
        if (chosen != config::display_mode()) {
            config::set_display_mode(chosen);
            changed = true;
        }
        if (screen > 0 && screen != config::display_monitor()) {
            config::set_display_monitor(screen);
            changed = true;
        }
        if (changed) request_display_mode_apply();  // applied on the window thread once this page has closed
    }
    o_Exit(self, edx);
    aircraft_track_reevaluate();  // leaving an Options page: no aircraft exist; apply a changed Tacview setting
    pilot_twist_apply_setting();
}

// --- Controls sub-menu: Mouse Pointer. Options.ini [ControlOptions] MouseMode: 1 = match the Windows cursor
// (default), 0 = stock. List index 0 = match, 1 = stock.
// Pilot Twist: [ControlOptions] PilotTwist: 0 = Off (default), 1 = On. List index = value.

Init_t o_CtlInit = nullptr;
Method_t o_CtlRefresh = nullptr;
Method_t o_CtlChanged = nullptr;
Method_t o_CtlExit = nullptr;
int g_mouse_selected = 0;  // list index (UI thread)
int g_twist_selected = 0;  // list index (UI thread)
bool g_twist_ready = false;  // Init filled the Pilot Twist list for this page instance; Exit writes only then
constexpr int kTwistOffset = 2;  // Pilot Twist list ID = Mouse Pointer list ID + 2

void* list_by_id(void* self, int id) {
    void* menu = *reinterpret_cast<void**>(static_cast<BYTE*>(self) + 4);
    if (!id || !menu) return nullptr;
    void* ctrl = nullptr;
    using GetItem_t = int(__fastcall*)(void*, void*, int, int, void**);
    reinterpret_cast<GetItem_t>(vtable_slot(menu, kMenuGetItem))(menu, nullptr, id, kItemTypeList, &ctrl);
    return ctrl;
}

void* twist_list(void* self) {
    const int id = g_mouse_list_id.load();
    return id ? list_by_id(self, id + kTwistOffset) : nullptr;
}

void __fastcall h_CtlInit(void* self, void* edx, int* a, int* b) {
    o_CtlInit(self, edx, a, b);
    void* list = list_by_id(self, g_mouse_list_id.load());
    if (!list) return;
    auto add = reinterpret_cast<AddItem_t>(vtable_slot(list, kListAddItem));
    add(list, nullptr, L"Match Windows cursor", 0);
    add(list, nullptr, L"Stock game pointer", 1);
    void* ini = options_ini();
    const int v = ini ? reinterpret_cast<IniReadInt_t>(at_rva(g_opts, kIniReadIntRva))(
                            ini, nullptr, L"ControlOptions", L"MouseMode", static_cast<int>(config::MouseMode::Cursor))
                      : static_cast<int>(config::MouseMode::Cursor);
    g_mouse_selected = v == static_cast<int>(config::MouseMode::Stock) ? 1 : 0;

    int t = 0;
    if (void* twist = twist_list(self)) {
        auto add_twist = reinterpret_cast<AddItem_t>(vtable_slot(twist, kListAddItem));
        add_twist(twist, nullptr, L"Off", 0);
        add_twist(twist, nullptr, L"On", 1);
        t = ini ? reinterpret_cast<IniReadInt_t>(at_rva(g_opts, kIniReadIntRva))(ini, nullptr, L"ControlOptions",
                                                                                L"PilotTwist", 0)
                : 0;
        g_twist_selected = t == 1 ? 1 : 0;
        g_twist_ready = true;
    }
    LOG("menu: Controls page opened, MouseMode=%d, PilotTwist=%d", v, t);
}

void __fastcall h_CtlRefresh(void* self, void* edx) {
    o_CtlRefresh(self, edx);
    if (void* list = list_by_id(self, g_mouse_list_id.load())) {
        reinterpret_cast<SetSel_t>(vtable_slot(list, kListSetSel))(list, nullptr, g_mouse_selected);
    }
    if (void* list = twist_list(self)) reinterpret_cast<SetSel_t>(vtable_slot(list, kListSetSel))(list, nullptr, g_twist_selected);
}

void __fastcall h_CtlChanged(void* self, void* edx) {
    o_CtlChanged(self, edx);
    if (void* list = list_by_id(self, g_mouse_list_id.load())) {
        const int sel = reinterpret_cast<GetSel_t>(vtable_slot(list, kListGetSel))(list, nullptr);
        if (sel == 0 || sel == 1) g_mouse_selected = sel;
    }
    if (void* list = twist_list(self)) {
        const int sel = reinterpret_cast<GetSel_t>(vtable_slot(list, kListGetSel))(list, nullptr);
        if (sel == 0 || sel == 1) g_twist_selected = sel;
    }
}

void __fastcall h_CtlExit(void* self, void* edx) {
    if (g_mouse_list_id.load()) {
        const auto mode = g_mouse_selected == 1 ? config::MouseMode::Stock : config::MouseMode::Cursor;
        if (void* ini = options_ini()) {
            reinterpret_cast<IniWriteInt_t>(at_rva(g_opts, kIniWriteIntRva))(ini, nullptr, L"ControlOptions",
                                                                              L"MouseMode", static_cast<int>(mode));
            LOG("menu: saved [ControlOptions] MouseMode=%d", static_cast<int>(mode));
        }
        if (mode != config::mouse_mode()) {
            config::set_mouse_mode(mode);
            request_display_mode_apply();  // re-applies the mouse's cooperative level on the window thread
        }
    }
    if (g_twist_ready) {
        g_twist_ready = false;
        const bool on = g_twist_selected == 1;
        if (void* ini = options_ini()) {
            reinterpret_cast<IniWriteInt_t>(at_rva(g_opts, kIniWriteIntRva))(ini, nullptr, L"ControlOptions",
                                                                              L"PilotTwist", on ? 1 : 0);
        }
        config::set_pilot_twist(on);  // logs a change
    }
    o_CtlExit(self, edx);
    aircraft_track_reevaluate();  // as in h_Exit
    pilot_twist_apply_setting();  // in the menus: no view update is running
}

// --- Gameplay sub-menu: Tacview Recording. Options.ini [OtherOptions] TacviewRecording: 0 = Off (default), 1 = On.
// The key keeps its section from the earlier Other page, so saved settings carry over. List index = value.

Init_t o_GplInit = nullptr;
Method_t o_GplRefresh = nullptr;
Method_t o_GplChanged = nullptr;
Method_t o_GplExit = nullptr;
int g_tac_selected = 0;     // list index (UI thread)
bool g_tac_ready = false;   // Init ran for this page instance; Exit writes only then

void __fastcall h_GplInit(void* self, void* edx, int* a, int* b) {
    o_GplInit(self, edx, a, b);
    void* list = list_by_id(self, g_tac_list_id.load());
    if (!list) return;
    auto add = reinterpret_cast<AddItem_t>(vtable_slot(list, kListAddItem));
    add(list, nullptr, L"Off", 0);
    add(list, nullptr, L"On", 1);
    void* ini = options_ini();
    const int v = ini ? reinterpret_cast<IniReadInt_t>(at_rva(g_opts, kIniReadIntRva))(ini, nullptr, L"OtherOptions",
                                                                                      L"TacviewRecording", 0)
                      : 0;
    g_tac_selected = v == 1 ? 1 : 0;
    g_tac_ready = true;
    LOG("menu: Gameplay page opened, TacviewRecording=%d", v);
}

void __fastcall h_GplRefresh(void* self, void* edx) {
    o_GplRefresh(self, edx);
    if (void* list = list_by_id(self, g_tac_list_id.load())) {
        reinterpret_cast<SetSel_t>(vtable_slot(list, kListSetSel))(list, nullptr, g_tac_selected);
    }
}

void __fastcall h_GplChanged(void* self, void* edx) {
    o_GplChanged(self, edx);
    if (void* list = list_by_id(self, g_tac_list_id.load())) {
        const int sel = reinterpret_cast<GetSel_t>(vtable_slot(list, kListGetSel))(list, nullptr);
        if (sel == 0 || sel == 1) g_tac_selected = sel;
    }
}

void __fastcall h_GplExit(void* self, void* edx) {
    if (g_tac_list_id.load() && g_tac_ready) {
        g_tac_ready = false;
        const bool on = g_tac_selected == 1;
        if (void* ini = options_ini()) {
            reinterpret_cast<IniWriteInt_t>(at_rva(g_opts, kIniWriteIntRva))(ini, nullptr, L"OtherOptions",
                                                                              L"TacviewRecording", on ? 1 : 0);
        }
        if ((on ? 1 : 0) != config::tacview_recording_peek()) LOG("menu: Tacview Recording set to %s", on ? "On" : "Off");
        config::set_tacview_recording(on);
    }
    o_GplExit(self, edx);
    aircraft_track_reevaluate();  // after config is updated: installs or removes the aircraft/weapon hooks
    pilot_twist_apply_setting();
}

SRWLOCK g_opts_lock = SRWLOCK_INIT;

using GetText_t = const wchar_t*(__fastcall*)(void*, void*, const wchar_t*, const wchar_t*);
using CopyText_t = int(__fastcall*)(void*, void*, const wchar_t*, const wchar_t*, wchar_t*, size_t);
GetText_t o_GetText = nullptr;
CopyText_t o_CopyText = nullptr;
std::atomic<bool> g_text_done{false};

const wchar_t* our_text(const wchar_t* key) {
    if (!key) return nullptr;
    if (wcscmp(key, kHintKey) == 0) return kHintText;
    if (wcscmp(key, kTacviewLabelKey) == 0) return L"Tacview Recording";
    if (wcscmp(key, kTacviewInfoKey) == 0) return L"Record every flight to a Tacview file in Saved Games.";
    if (wcscmp(key, L"TXT_LAZY_SCREEN_INFO") == 0) return L"Select the monitor to display the game on.";
    if (wcscmp(key, L"TXT_LAZY_PILOT_TWIST_INFO") == 0) return L"Looking far behind turns the view further and leans a little.";
    // ~70 characters, like the longer stock hints; one line.
    if (wcscmp(key, L"TXT_LAZY_MOUSE_INFO") == 0) return L"Match Windows puts the game's pointer exactly on the Windows cursor.";
    return nullptr;
}

const wchar_t* __fastcall h_GetText(void* self, void* edx, const wchar_t* file, const wchar_t* key) {
    if (const wchar_t* t = our_text(key)) return t;
    return o_GetText(self, edx, file, key);
}

int __fastcall h_CopyText(void* self, void* edx, const wchar_t* file, const wchar_t* key, wchar_t* buf, size_t n) {
    if (const wchar_t* t = our_text(key); t && buf && n) {
        wcsncpy_s(buf, n, t, _TRUNCATE);
        return 1;
    }
    return o_CopyText(self, edx, file, key, buf, n);
}

void hook_text_service() {
    if (g_text_done.load()) return;
    HMODULE text = GetModuleHandleW(L"TextUSEnglish.dll");
    if (!text || g_text_done.exchange(true)) return;
    if (!is_expected_build(text, kTextTimestamp, kTextImageSize, "TextUSEnglish.dll")) {
        LOG("menu: Display Mode hint will show its key (text DLL not recognised)");
        return;
    }
    create_and_enable(at_rva(text, kGetTextRva), reinterpret_cast<void*>(&h_GetText), reinterpret_cast<void**>(&o_GetText),
                      "TextUSEnglish GetText");
    create_and_enable(at_rva(text, kCopyTextRva), reinterpret_cast<void*>(&h_CopyText),
                      reinterpret_cast<void**>(&o_CopyText), "TextUSEnglish CopyText");
}

void hook_options_screen(HMODULE opts) {
    create_and_enable(at_rva(opts, kGfxInitRva), reinterpret_cast<void*>(&h_Init), reinterpret_cast<void**>(&o_Init),
                      "OptionsScreen Graphics Init");
    create_and_enable(at_rva(opts, kGfxRefreshRva), reinterpret_cast<void*>(&h_Refresh),
                      reinterpret_cast<void**>(&o_Refresh), "OptionsScreen Graphics Refresh");
    create_and_enable(at_rva(opts, kGfxChangedRva), reinterpret_cast<void*>(&h_Changed),
                      reinterpret_cast<void**>(&o_Changed), "OptionsScreen Graphics Changed");
    create_and_enable(at_rva(opts, kGfxExitRva), reinterpret_cast<void*>(&h_Exit), reinterpret_cast<void**>(&o_Exit),
                      "OptionsScreen Graphics Exit");
    create_and_enable(at_rva(opts, kCtlInitRva), reinterpret_cast<void*>(&h_CtlInit),
                      reinterpret_cast<void**>(&o_CtlInit), "OptionsScreen Controls Init");
    create_and_enable(at_rva(opts, kCtlRefreshRva), reinterpret_cast<void*>(&h_CtlRefresh),
                      reinterpret_cast<void**>(&o_CtlRefresh), "OptionsScreen Controls Refresh");
    create_and_enable(at_rva(opts, kCtlChangedRva), reinterpret_cast<void*>(&h_CtlChanged),
                      reinterpret_cast<void**>(&o_CtlChanged), "OptionsScreen Controls Changed");
    create_and_enable(at_rva(opts, kCtlExitRva), reinterpret_cast<void*>(&h_CtlExit),
                      reinterpret_cast<void**>(&o_CtlExit), "OptionsScreen Controls Exit");
    create_and_enable(at_rva(opts, kGplInitRva), reinterpret_cast<void*>(&h_GplInit),
                      reinterpret_cast<void**>(&o_GplInit), "OptionsScreen Gameplay Init");
    create_and_enable(at_rva(opts, kGplRefreshRva), reinterpret_cast<void*>(&h_GplRefresh),
                      reinterpret_cast<void**>(&o_GplRefresh), "OptionsScreen Gameplay Refresh");
    create_and_enable(at_rva(opts, kGplChangedRva), reinterpret_cast<void*>(&h_GplChanged),
                      reinterpret_cast<void**>(&o_GplChanged), "OptionsScreen Gameplay Changed");
    create_and_enable(at_rva(opts, kGplExitRva), reinterpret_cast<void*>(&h_GplExit),
                      reinterpret_cast<void**>(&o_GplExit), "OptionsScreen Gameplay Exit");
}

// Removes the 12 OptionsScreen hooks at this base; entries that don't exist are fine.
void remove_options_hooks(HMODULE base) {
    for (DWORD rva : {kGfxInitRva, kGfxRefreshRva, kGfxChangedRva, kGfxExitRva, kCtlInitRva, kCtlRefreshRva,
                      kCtlChangedRva, kCtlExitRva, kGplInitRva, kGplRefreshRva, kGplChangedRva, kGplExitRva}) {
        MH_STATUS s = MH_RemoveHook(at_rva(base, rva));
        (void)s;  // MH_ERROR_NOT_CREATED is expected
    }
}

bool g_path_too_long = false;  // set by old_opts_still_loaded when the module path was truncated

// True if base is still the loaded OptionsScreen.dll image (not freed, not replaced by another DLL).
bool old_opts_still_loaded(HMODULE base) {
    g_path_too_long = false;
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(base), &h) ||
        h != base) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(base, path, MAX_PATH);
    static constexpr wchar_t kName[] = L"\\OptionsScreen.dll";
    constexpr DWORD kLen = static_cast<DWORD>(_countof(kName) - 1);
    if (n >= MAX_PATH) {
        // Truncated: the name can't be verified, so don't touch that image.
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("menu: OptionsScreen.dll path too long to verify; dropping its hooks without restoring");
        }
        g_path_too_long = true;
        return false;
    }
    if (n < kLen) return false;
    return CompareStringOrdinal(path + n - kLen, static_cast<int>(kLen), kName, static_cast<int>(kLen), TRUE) ==
           CSTR_EQUAL;
}

}  // namespace

bool install_menu_hooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!is_expected_build(exe, kExeTimestamp, kExeImageSize, "StrikeFighters2.exe")) return false;
    // Created here, enabled with MH_ALL_HOOKS by DllMain.
    bool ok = MH_CreateHook(at_rva(exe, kFileOpenRva), reinterpret_cast<void*>(&h_FileOpen),
                            reinterpret_cast<void**>(&o_FileOpen)) == MH_OK;
    ok &= MH_CreateHook(at_rva(exe, kFileCloseRva), reinterpret_cast<void*>(&h_FileClose),
                        reinterpret_cast<void**>(&o_FileClose)) == MH_OK;
    ok &= MH_CreateHook(at_rva(exe, kFileReadRva), reinterpret_cast<void*>(&h_FileRead),
                        reinterpret_cast<void**>(&o_FileRead)) == MH_OK;
    LOG("menu: file service hooks %s", ok ? "created" : "FAILED");
    return ok;
}

void menu_on_modules_loaded() {
    // MenuSystem loads OptionsScreen.dll when the Options screen opens and frees it when it closes,
    // so every load is a fresh, unpatched image (possibly at the same address). Re-hook whenever
    // the loaded image does not carry our patch.
    hook_text_service();  // TextUSEnglish.dll stays loaded; hooked once
    HMODULE opts = GetModuleHandleW(L"OptionsScreen.dll");
    if (!opts) return;
    auto* init = static_cast<const BYTE*>(at_rva(opts, kGfxInitRva));
    if (opts == g_opts && *init == 0xE9) return;  // already hooked (MinHook writes a JMP rel32)

    AcquireSRWLockExclusive(&g_opts_lock);
    if (!(opts == g_opts && *init == 0xE9)) {
        // Verify the new image first: every MH_RemoveHook below restores original bytes, which is
        // only safe on an OptionsScreen.dll of the build we analysed.
        const bool build_ok = is_expected_build(opts, kOptsTimestamp, kOptsImageSize, "OptionsScreen.dll");
        if (g_opts) {
            // Forget the hooks on the previous image. MinHook restores the original bytes on removal,
            // so only do that while the old base is still our OptionsScreen.dll; otherwise those
            // bytes would be written into whatever is mapped there now.
            if (build_ok && old_opts_still_loaded(g_opts)) {
                remove_options_hooks(g_opts);
            } else if (!build_ok) {
                LOG("menu: new OptionsScreen.dll image unverified; not removing any hooks");
            } else if (!g_path_too_long) {
                LOG("menu: old OptionsScreen.dll image gone, dropping its hooks without restoring");
            }
            g_opts = nullptr;
        }
        if (build_ok) {
            // A previous load at this base may have left entries in MinHook (its image was freed while
            // they were registered); remove them so MH_CreateHook doesn't fail with ALREADY_CREATED.
            // Safe: this is a verified OptionsScreen.dll, so the restored bytes are identical.
            remove_options_hooks(opts);
            g_opts = opts;
            hook_options_screen(opts);
        }
    }
    ReleaseSRWLockExclusive(&g_opts_lock);
}

}  // namespace LazyWorks::SF2Patch::hooks
