// The content hash the lobby compares between host and clients (L4): a shared mission only plays the same on PCs with
// the same game content. Hashes the exe's build, every .cat in the game folder (path and size), and for each folder
// under the player's Objects\Aircraft, Objects\Weapons, Objects\GroundObject and Terrains (mods and DLC) its name and
// the names and sizes of the .ini files directly in it (the files that define an object or theatre). Cheap: only
// directory listings are read. Computed again for each session, so content changed between sessions counts.
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "config.h"
#include "hooks/hooks.h"
#include "log.h"
#include "net/protocol.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Every .cat under dir (two levels: Flight\, Objects\, Terrains\, Menu\ ...), as "relative path:size".
void list_cats(const std::wstring& root, const std::wstring& rel, int depth, std::vector<std::wstring>* out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                list_cats(root, rel + name + L"\\", depth - 1, out);
        } else if (name.size() > 4 && lower(name.substr(name.size() - 4)) == L".cat") {
            const unsigned long long size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out->push_back(lower(rel + name) + L":" + std::to_wstring(size));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The .ini files directly in dir, as "prefix/name:size".
void list_inis(const std::wstring& dir, const std::wstring& prefix, std::vector<std::wstring>* out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const unsigned long long size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out->push_back(prefix + L"/" + lower(fd.cFileName) + L":" + std::to_wstring(size));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void list_dirs(const std::wstring& dir, const wchar_t* tag, std::vector<std::wstring>* out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring prefix = std::wstring(tag) + L"/" + lower(name);
        out->push_back(prefix);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) list_inis(dir + L"\\" + name, prefix, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

uint32_t content_hash() {
    std::vector<std::wstring> items;
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring game(exe);
        game.resize(game.find_last_of(L'\\') + 1);
        list_cats(game, L"", 2, &items);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const BYTE*>(dos) + dos->e_lfanew);
        items.push_back(L"exe:" + std::to_wstring(nt->FileHeader.TimeDateStamp));
    }
    const size_t cats = items.size();
    const std::wstring user = config::game_data_dir();
    if (!user.empty()) {
        list_dirs(user + L"\\Objects\\Aircraft", L"aircraft", &items);
        list_dirs(user + L"\\Objects\\Weapons", L"weapons", &items);
        list_dirs(user + L"\\Objects\\GroundObject", L"ground", &items);
        list_dirs(user + L"\\Terrains", L"terrains", &items);
    }
    std::sort(items.begin(), items.end());
    uint32_t h = 0x811C9DC5u;
    auto mix = [&](uint32_t b) {
        h ^= b;
        h *= 0x01000193u;
    };
    mix(net::kVersion);
    for (const std::wstring& s : items) {
        for (wchar_t c : s) {
            mix(static_cast<uint32_t>(c) & 0xff);
            mix(static_cast<uint32_t>(c) >> 8);
        }
        mix(0);
    }
    const uint32_t hash = h ? h : 1u;
    LOG("lobby: content hash 0x%08lx (%zu game files, %zu mod folders and files)", static_cast<unsigned long>(hash), cats,
        items.size() - cats);
    return hash;
}

}  // namespace LazyWorks::SF2Patch::hooks
