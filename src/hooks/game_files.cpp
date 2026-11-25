#include "hooks/game_files.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <new>

#include "hooks/hook_util.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

// StrikeFighters2.exe, July 2013 build (the same stamp as menu_hooks.cpp). See lazy-sf2-recon: recon-notes §21.4.
constexpr DWORD kExeTimestamp = 0x51d22110, kExeImageSize = 0x62000;
constexpr DWORD kRvaFileManagerGlobal = 0x49ccc, kRvaFileManagerVtable = 0x31d0c;
constexpr size_t kSlotOpen = 0x10, kSlotClose = 0x14, kSlotRead = 0x18, kSlotSize = 0x30;  // byte offsets in the vtable
constexpr unsigned kChunk = 64 * 1024;

using Size_t = int(__fastcall*)(void*, void*, const wchar_t*, int);
using Open_t = int(__fastcall*)(void*, void*, const wchar_t*, int);
using Read_t = int(__fastcall*)(void*, void*, int, void*, unsigned);
using Close_t = void(__fastcall*)(void*, void*, int);

struct Service {
    void* self = nullptr;
    Size_t size = nullptr;
    Open_t open = nullptr;
    Read_t read = nullptr;
    Close_t close = nullptr;
};

std::atomic<bool> g_build_checked{false}, g_build_ok{false};
std::atomic<bool> g_logged_service{false};

// The file manager object, checked against its vtable. No C++ objects here (SEH).
bool find_service(HMODULE exe, Service* s) {
    __try {
        void* svc = *static_cast<void**>(at_rva(exe, kRvaFileManagerGlobal));
        if (!svc) return false;
        void** vt = *static_cast<void***>(svc);
        if (vt != at_rva(exe, kRvaFileManagerVtable)) return false;
        const auto slot = [&](size_t off) { return vt[off / sizeof(void*)]; };
        s->self = svc;
        s->open = reinterpret_cast<Open_t>(slot(kSlotOpen));
        s->close = reinterpret_cast<Close_t>(slot(kSlotClose));
        s->read = reinterpret_cast<Read_t>(slot(kSlotRead));
        s->size = reinterpret_cast<Size_t>(slot(kSlotSize));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Each call to the game is guarded; no C++ objects here (SEH). Failure is -1 (or 0 for a size).
int call_size(const Service& s, const wchar_t* name) {
    __try {
        return s.size(s.self, nullptr, name, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int call_open(const Service& s, const wchar_t* name) {
    __try {
        return s.open(s.self, nullptr, name, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int call_read(const Service& s, int h, void* buf, unsigned n) {
    __try {
        return s.read(s.self, nullptr, h, buf, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void call_close(const Service& s, int h) {
    __try {
        s.close(s.self, nullptr, h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

bool read_game_file(const std::wstring& full_path, std::vector<uint8_t>& out) {
    out.clear();
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!g_build_checked.load(std::memory_order_acquire)) {
        g_build_ok.store(is_expected_build(exe, kExeTimestamp, kExeImageSize, "StrikeFighters2.exe"));
        g_build_checked.store(true, std::memory_order_release);
    }
    if (!g_build_ok.load()) return false;
    Service s;
    if (!find_service(exe, &s)) {
        if (!g_logged_service.exchange(true)) LOG("game files: the exe's file manager is not as expected");
        return false;
    }
    const wchar_t* name = full_path.c_str();
    try {
        const int size = call_size(s, name);
        if (size < 0 || static_cast<size_t>(size) > kMaxGameFileBytes) return false;
        out.resize(size > 0 ? static_cast<size_t>(size) : kChunk);  // before Open: a throw cannot leak the handle
        const int h = call_open(s, name);
        if (h == -1) {
            out.clear();
            return false;
        }
        bool ok = true;
        size_t have = 0;
        if (size > 0) {
            // Read until all of it is in; a read of 0 or less before that is a failure.
            while (have < out.size()) {
                const size_t want = std::min<size_t>(out.size() - have, kChunk);
                const int got = call_read(s, h, out.data() + have, static_cast<unsigned>(want));
                if (got <= 0) break;
                have += static_cast<size_t>(got);
            }
            ok = have == out.size();
        } else {
            // The size query found nothing (it may not see every file): read until the end, up to the cap.
            for (;;) {
                if (have == out.size()) {
                    if (out.size() + kChunk > kMaxGameFileBytes) {
                        ok = false;
                        break;
                    }
                    try {
                        out.resize(out.size() + kChunk);
                    } catch (...) {
                        ok = false;
                        break;
                    }
                }
                const int got = call_read(s, h, out.data() + have, static_cast<unsigned>(out.size() - have));
                if (got <= 0) break;
                have += static_cast<size_t>(got);
            }
            out.resize(have);
            ok = ok && have > 0;
        }
        call_close(s, h);
        if (!ok) out.clear();
        return ok;
    } catch (...) {  // std::bad_alloc
        out.clear();
        return false;
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
