#include "log.h"

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace LazyWorks::SF2Patch::log {
namespace {

CRITICAL_SECTION g_lock;
FILE* g_file = nullptr;

}  // namespace

std::wstring log_path(const std::wstring& path) {
    wchar_t profile[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return path;
    while (n > 0 && profile[n - 1] == L'\\') profile[--n] = 0;
    if (n == 0 || path.size() < n) return path;
    if (CompareStringOrdinal(path.c_str(), static_cast<int>(n), profile, static_cast<int>(n), TRUE) != CSTR_EQUAL) {
        return path;
    }
    if (path.size() > n && path[n] != L'\\') return path;  // a sibling like <profile>2 is not under the profile
    return L"%USERPROFILE%" + path.substr(n);
}

void open() {
    InitializeCriticalSection(&g_lock);

    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"lazysf2.log");

    g_file = _wfsopen(path, L"w", _SH_DENYNO);  // truncate on every start; readable while running
    if (g_file) return;
    // The game folder is not writable (an install under Program Files run without elevation, and no VirtualStore
    // copy): %LOCALAPPDATA%\LazySF2Patch\lazysf2.log instead. Plain kernel32 calls only: this runs under the loader lock.
    const DWORD err = GetLastError();
    wchar_t local[MAX_PATH] = {};
    const DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (ln == 0 || ln >= MAX_PATH - 40) return;
    std::wstring dir = std::wstring(local) + L"\\LazySF2Patch";
    CreateDirectoryW(dir.c_str(), nullptr);
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    const std::wstring fallback = dir + L"\\lazysf2.log";
    g_file = _wfsopen(fallback.c_str(), L"w", _SH_DENYNO);
    if (!g_file) return;
    char why[512];
    std::snprintf(why, sizeof(why), "log: %ls is not writable (error %lu); logging to %ls instead", log_path(path).c_str(),
                  static_cast<unsigned long>(err), log_path(fallback).c_str());
    std::fprintf(g_file, "%s\n", why);
    std::fflush(g_file);
}

void close() {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    DeleteCriticalSection(&g_lock);
}

void write(const char* fmt, ...) {
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);

    EnterCriticalSection(&g_lock);
    fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

}  // namespace LazyWorks::SF2Patch::log
