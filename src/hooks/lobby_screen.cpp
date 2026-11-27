// Multiplayer lobby screen: this DLL is the screen's menu code (MenuCodeDLL in the spliced MenuSystem.ini), so the menu
// system calls InitMenuCode/ExitMenuCode here and drives our handler like a stock screen's. The last address is kept in
// Options.ini [OtherOptions] LobbyAddress, through the game's own Options.ini object.
// See lazy-sf2-recon: recon-notes §28.
#include <windows.h>

#include <cwchar>
#include <string>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/lobby_splice.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kExeTimestamp = 0x51d22110, kExeImageSize = 0x62000;
constexpr DWORD kOptObjectRva = 0x3ce28;     // the OPT service object (shared Options.ini)
constexpr DWORD kOptVtableRva = 0x31c1c;
constexpr DWORD kIniGetStringRva = 0x11d20;  // ini GetString(section, key, default, out, size, mode)
constexpr DWORD kIniSetStringRva = 0x117d0;  // ini SetString(section, key, value)
constexpr int kOptAcquire = 0x08 / 4;        // bool Acquire(ini** out)
constexpr int kOptRelease = 0x0c / 4;        // void Release(ini** p)
constexpr int kOptReleaseSave = 0x10 / 4;    // int Release(ini** p), writing Options.ini first

constexpr DWORD kMnuTimestamp = 0x51d21fc1, kMnuImageSize = 0x41000;  // Core\MenuSystem.dll
constexpr DWORD kMnuVtableRva = 0x30274;
constexpr int kSysGetService = 0x08 / 4;      // GetService(tag, void** out)
constexpr int kSysReleaseService = 0x0c / 4;  // ReleaseService(tag, void** p)
constexpr int kMnuItemUnderPointer = 0x10 / 4;
constexpr int kMnuPostCommand = 0x24 / 4;  // PostCommand(cmd, a, b)
constexpr int kMnuGetItem = 0x50 / 4;      // int GetItem(item, type, void** iface)
constexpr int kIfaceSelection = 2, kIfaceText = 4, kIfaceEdit = 7;
constexpr int kTextSet = 0x0c / 4;  // TEXT SetText(text)
constexpr int kEditSet = 0x18 / 4;  // EDIT SetText(text)
constexpr int kEditGet = 0x1c / 4;  // EDIT GetText(buf, n)
constexpr int kCmdGoTo = 3;         // go to the item's DestinationMenu
constexpr int kCmdArg = 10;         // third PostCommand argument, as the stock screens pass it

constexpr wchar_t kAddressSection[] = L"OtherOptions";
constexpr wchar_t kAddressKey[] = L"LobbyAddress";

using Acquire_t = int(__fastcall*)(void* opt, void* edx, void** out);
using Release_t = void(__fastcall*)(void* opt, void* edx, void** p);
using ReleaseSave_t = int(__fastcall*)(void* opt, void* edx, void** p);
using IniGetString_t = void(__fastcall*)(void* ini, void* edx, const wchar_t* section, const wchar_t* key,
                                         const wchar_t* def, wchar_t* out, size_t n, int mode);
using IniSetString_t = int(__fastcall*)(void* ini, void* edx, const wchar_t* section, const wchar_t* key,
                                        const wchar_t* value);
using Service_t = int(__fastcall*)(void* sys, void* edx, const wchar_t* tag, void** p);
using ItemUnderPointer_t = int(__fastcall*)(void* mnu, void* edx);
using PostCommand_t = void(__fastcall*)(void* mnu, void* edx, int cmd, int a, int b);
using GetItem_t = int(__fastcall*)(void* mnu, void* edx, int item, int type, void** iface);
using SetText_t = void(__fastcall*)(void* iface, void* edx, const wchar_t* text);
using GetText_t = void(__fastcall*)(void* iface, void* edx, wchar_t* buf, size_t n);

// Menu thread only: the menu system calls InitMenuCode, the handler and ExitMenuCode on its own thread.
void* g_sys = nullptr;
void* g_mnu = nullptr;
std::wstring g_saved_address;  // what Options.ini holds (as last read or written)

/// <summary>The OPT service object if the exe is the expected build and the object is as analysed, else nullptr.</summary>
void* opt_object() {
    static int state = 0;  // 0 = unchecked, 1 = ok, -1 = not usable
    HMODULE exe = GetModuleHandleW(nullptr);
    if (state == 0) {
        state = is_expected_build(exe, kExeTimestamp, kExeImageSize, "StrikeFighters2.exe") ? 1 : -1;
        if (state == 1 && *static_cast<void**>(at_rva(exe, kOptObjectRva)) != at_rva(exe, kOptVtableRva)) {
            LOG("lobby: Options.ini service not as expected; the address is not saved");
            state = -1;
        }
    }
    return state == 1 ? at_rva(exe, kOptObjectRva) : nullptr;
}

/// <summary>Reads a string from the game's Options.ini object; false if it is not available.</summary>
bool read_option(const wchar_t* section, const wchar_t* key, wchar_t* out, size_t n) {
    out[0] = L'\0';
    void* opt = opt_object();
    if (!opt) return false;
    void* ini = nullptr;
    if (!reinterpret_cast<Acquire_t>(vtable_slot(opt, kOptAcquire))(opt, nullptr, &ini) || !ini) return false;
    HMODULE exe = GetModuleHandleW(nullptr);
    reinterpret_cast<IniGetString_t>(at_rva(exe, kIniGetStringRva))(ini, nullptr, section, key, L"", out, n, 0);
    reinterpret_cast<Release_t>(vtable_slot(opt, kOptRelease))(opt, nullptr, &ini);
    return true;
}

/// <summary>Writes a string to the game's Options.ini object, and the game writes the file.</summary>
bool write_option(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    void* opt = opt_object();
    if (!opt) return false;
    void* ini = nullptr;
    if (!reinterpret_cast<Acquire_t>(vtable_slot(opt, kOptAcquire))(opt, nullptr, &ini) || !ini) return false;
    HMODULE exe = GetModuleHandleW(nullptr);
    const int set = reinterpret_cast<IniSetString_t>(at_rva(exe, kIniSetStringRva))(ini, nullptr, section, key, value);
    reinterpret_cast<ReleaseSave_t>(vtable_slot(opt, kOptReleaseSave))(opt, nullptr, &ini);
    return set != 0;
}

void* item_iface(int item, int type) {
    void* iface = nullptr;
    if (!g_mnu) return nullptr;
    reinterpret_cast<GetItem_t>(vtable_slot(g_mnu, kMnuGetItem))(g_mnu, nullptr, item, type, &iface);
    return iface;
}

void set_text(int item, const wchar_t* text) {
    if (void* t = item_iface(item, kIfaceText)) reinterpret_cast<SetText_t>(vtable_slot(t, kTextSet))(t, nullptr, text);
}

void set_status(const wchar_t* text) {
    set_text(lobby_item::kStatus, text);
    LOG("lobby: status \"%ls\"", text);
}

std::wstring read_address() {
    wchar_t buf[kLobbyAddressChars + 1] = {};
    if (void* e = item_iface(lobby_item::kAddress, kIfaceEdit)) {
        reinterpret_cast<GetText_t>(vtable_slot(e, kEditGet))(e, nullptr, buf, _countof(buf));
    }
    buf[kLobbyAddressChars] = L'\0';
    std::wstring s = buf;
    const size_t first = s.find_first_not_of(L" \t");
    const size_t last = s.find_last_not_of(L" \t");
    return first == std::wstring::npos ? std::wstring() : s.substr(first, last - first + 1);
}

/// <summary>Reads the Address field, logs it, and saves it to Options.ini when it changed.</summary>
std::wstring take_address() {
    std::wstring a = read_address();
    LOG("lobby: address field reads \"%ls\"", a.c_str());
    if (a != g_saved_address) {
        if (write_option(kAddressSection, kAddressKey, a.c_str())) {
            g_saved_address = a;
            LOG("lobby: saved [%ls] %ls=%ls", kAddressSection, kAddressKey, a.c_str());
        } else {
            LOG("lobby: could not save the address");
        }
    }
    return a;
}

void post(int cmd, int a) {
    reinterpret_cast<PostCommand_t>(vtable_slot(g_mnu, kMnuPostCommand))(g_mnu, nullptr, cmd, a, kCmdArg);
}

/// <summary>The stock screens' click test: a left-button release over a button, or Esc (reported as item 1).</summary>
int clicked_item(const int* ev) {
    int item = 0;
    if (ev[0] == 3) {
        if ((ev[3] & 0xf) == 0 && ev[2] == 0) {
            item = reinterpret_cast<ItemUnderPointer_t>(vtable_slot(g_mnu, kMnuItemUnderPointer))(g_mnu, nullptr);
        }
    } else if (ev[0] == 1 && (ev[3] & 0xf) != 0 && ev[2] == 1) {
        item = lobby_item::kBack;
    }
    return item > 0 && item_iface(item, kIfaceSelection) ? item : 0;
}

/// <summary>The handler object the menu system calls (thiscall slots: 0 frame, 2 refresh, 3 input event).</summary>
class LobbyHandler {
public:
    virtual void frame() {}
    virtual void unused() {}
    virtual void refresh() {}
    virtual void handle_event(const int* ev) {
        if (!g_mnu || !ev) return;
        switch (clicked_item(ev)) {
            case lobby_item::kBack:
                take_address();
                LOG("lobby: Back");
                post(kCmdGoTo, lobby_item::kBack);
                break;
            case lobby_item::kHost: {
                const std::wstring a = take_address();
                LOG("lobby: Host clicked (address \"%ls\"); hosting is not wired up yet", a.c_str());
                set_status(L"Hosting is not available yet.");
                break;
            }
            case lobby_item::kJoin: {
                const std::wstring a = take_address();
                LOG("lobby: Join clicked (address \"%ls\"); joining is not wired up yet", a.c_str());
                set_status(a.empty() ? L"Type the host's address first." : L"Joining is not available yet.");
                break;
            }
            default:
                break;
        }
    }
};
LobbyHandler g_handler;

void pin_this_module() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE self = nullptr;
    const BOOL ok = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                       reinterpret_cast<LPCWSTR>(&pin_this_module), &self);
    LOG("lobby: mod DLL pinned as menu code: %s", ok ? "ok" : "FAILED");
}

}  // namespace

std::wstring lobby_menu_code_path() {
    static int state = 0;  // 0 = undecided, 1 = ok, -1 = not available
    static std::wstring path;
    if (state != 0) return path;
    HMODULE mnu = GetModuleHandleW(L"MenuSystem.dll");
    if (!mnu) return {};  // not loaded yet: decide on a later read
    state = -1;
    if (!is_expected_build(GetModuleHandleW(nullptr), kExeTimestamp, kExeImageSize, "StrikeFighters2.exe") ||
        !is_expected_build(mnu, kMnuTimestamp, kMnuImageSize, "MenuSystem.dll")) {
        LOG("lobby: not available (unexpected build); the Multiplayer button stays hidden");
        return path;
    }
    HMODULE self = nullptr;
    wchar_t buf[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&lobby_menu_code_path), &self) ||
        GetModuleFileNameW(self, buf, MAX_PATH) >= MAX_PATH) {
        LOG("lobby: not available (mod DLL path unknown); the Multiplayer button stays hidden");
        return path;
    }
    path = buf;
    state = 1;
    LOG("lobby: available, menu code %ls", path.c_str());
    return path;
}

}  // namespace LazyWorks::SF2Patch::hooks

using namespace LazyWorks::SF2Patch::hooks;

/// <summary>Menu-code entry point: the menu system calls it on the lobby screen's first frame.</summary>
extern "C" __declspec(dllexport) void __cdecl InitMenuCode(void* sys, void** handler) {
    if (handler) *handler = nullptr;
    if (!sys || !handler) return;
    g_sys = sys;
    g_mnu = nullptr;
    reinterpret_cast<Service_t>(vtable_slot(sys, kSysGetService))(sys, nullptr, L"MNU", &g_mnu);
    HMODULE mnu_dll = GetModuleHandleW(L"MenuSystem.dll");
    if (!g_mnu || !mnu_dll || *static_cast<void**>(g_mnu) != at_rva(mnu_dll, kMnuVtableRva)) {
        LOG("lobby: menu service not as expected; the lobby screen is inert");
        g_mnu = nullptr;
        return;
    }
    pin_this_module();

    wchar_t callsign[64] = {};
    read_option(L"Pilot", L"Callsign", callsign, _countof(callsign));
    set_text(lobby_item::kCallsign, callsign);

    wchar_t address[kLobbyAddressChars + 1] = {};
    const bool have_options = read_option(kAddressSection, kAddressKey, address, _countof(address));
    g_saved_address = address;
    if (void* e = item_iface(lobby_item::kAddress, kIfaceEdit)) {
        reinterpret_cast<SetText_t>(vtable_slot(e, kEditSet))(e, nullptr, address);
    }
    set_text(lobby_item::kStatus, L"Not connected.");
    *handler = &g_handler;
    LOG("lobby: screen opened (callsign \"%ls\", saved address \"%ls\"%s)", callsign, address,
        have_options ? "" : ", Options.ini not available");
}

/// <summary>Menu-code exit: the menu system calls it when the lobby screen closes, before it frees the DLL reference.</summary>
extern "C" __declspec(dllexport) void __cdecl ExitMenuCode() {
    if (g_sys && g_mnu) reinterpret_cast<Service_t>(vtable_slot(g_sys, kSysReleaseService))(g_sys, nullptr, L"MNU", &g_mnu);
    g_mnu = nullptr;
    g_sys = nullptr;
    LOG("lobby: screen closed");
}
