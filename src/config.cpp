#include "config.h"

#include <windows.h>

#include "log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace lazysf2::config {
namespace {

Config g_config;

}  // namespace

void load() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    wchar_t* slash = (n && n < MAX_PATH) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"lazysf2.ini");

    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"Mouse", L"Mode", L"Stock", mode, 32, path);
    if (_wcsicmp(mode, L"Cursor") == 0) {
        g_config.mouse = MouseMode::Cursor;
    } else if (_wcsicmp(mode, L"Stock") != 0) {
        LOG("config: unknown [Mouse] Mode=%ls, using Stock", mode);
    }
    LOG("config: borderless=%d mouse=%s", g_config.borderless,
        g_config.mouse == MouseMode::Cursor ? "Cursor" : "Stock");
}

const Config& get() { return g_config; }

}  // namespace lazysf2::config
