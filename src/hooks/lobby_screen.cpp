// Multiplayer lobby screen: this DLL is the screen's menu code (MenuCodeDLL in the spliced MenuSystem.ini), so the menu
// system calls InitMenuCode/ExitMenuCode here and drives our handler like a stock screen's.
// See lazy-sf2-recon: recon-notes §28.
#include <windows.h>

#include <string>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/lobby_splice.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kExeTimestamp = 0x51d22110, kExeImageSize = 0x62000;
constexpr DWORD kMnuTimestamp = 0x51d21fc1, kMnuImageSize = 0x41000;  // Core\MenuSystem.dll
constexpr DWORD kMnuVtableRva = 0x30274;
constexpr int kSysGetService = 0x08 / 4;      // GetService(tag, void** out)
constexpr int kSysReleaseService = 0x0c / 4;  // ReleaseService(tag, void** p)
constexpr int kMnuItemUnderPointer = 0x10 / 4;
constexpr int kMnuPostCommand = 0x24 / 4;  // PostCommand(cmd, a, b)
constexpr int kMnuGetItem = 0x50 / 4;      // int GetItem(item, type, void** iface)
constexpr int kIfaceSelection = 2;
constexpr int kCmdGoTo = 3;  // go to the item's DestinationMenu
constexpr int kCmdArg = 10;  // third PostCommand argument, as the stock screens pass it

using Service_t = int(__fastcall*)(void* sys, void* edx, const wchar_t* tag, void** p);
using ItemUnderPointer_t = int(__fastcall*)(void* mnu, void* edx);
using PostCommand_t = void(__fastcall*)(void* mnu, void* edx, int cmd, int a, int b);
using GetItem_t = int(__fastcall*)(void* mnu, void* edx, int item, int type, void** iface);

// Menu thread only: the menu system calls InitMenuCode, the handler and ExitMenuCode on its own thread.
void* g_sys = nullptr;
void* g_mnu = nullptr;

void* item_iface(int item, int type) {
    void* iface = nullptr;
    if (!g_mnu) return nullptr;
    reinterpret_cast<GetItem_t>(vtable_slot(g_mnu, kMnuGetItem))(g_mnu, nullptr, item, type, &iface);
    return iface;
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
                LOG("lobby: Back");
                post(kCmdGoTo, lobby_item::kBack);
                break;
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
    *handler = &g_handler;
    LOG("lobby: screen opened");
}

/// <summary>Menu-code exit: the menu system calls it when the lobby screen closes, before it frees the DLL reference.</summary>
extern "C" __declspec(dllexport) void __cdecl ExitMenuCode() {
    if (g_sys && g_mnu) reinterpret_cast<Service_t>(vtable_slot(g_sys, kSysReleaseService))(g_sys, nullptr, L"MNU", &g_mnu);
    g_mnu = nullptr;
    g_sys = nullptr;
    LOG("lobby: screen closed");
}
