#include "hooks/terrain_export.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <set>
#include <thread>
#include <vector>

#include "acmi/tacview_terrain.h"
#include "hooks/hook_util.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

// TerrainEngine.dll, July 2013 build. See lazy-sf2-recon: recon-notes §19.9.
constexpr DWORD kStamp = 0x51d21fa0, kImageSize = 0x46000;
constexpr DWORD kRvaTerObject = 0x3f650, kRvaTerVtable = 0x3543c;
constexpr size_t kOffKind = 0x08, kOffWidth = 0x78, kOffHeight = 0x7a, kOffSpacing = 0x80, kOffSamples = 0xa4,
                 kOffLoaded = 0x918, kOffManager = 0x04, kOffCurrent = 0x14, kOffBaseName = 0x00, kOffFullName = 0x04;
constexpr size_t kNameChars = 260;
constexpr int kMaxDim = 8192;

struct Raw {
    wchar_t base[kNameChars + 1];
    wchar_t full[kNameChars + 1];
    int w, h;
    double spacing;
    const uint16_t* samples;
};

bool copy_wstr(wchar_t* dst, const wchar_t* src) {
    if (!src) return false;
    for (size_t i = 0; i < kNameChars; ++i) {
        dst[i] = src[i];
        if (!src[i]) return true;
    }
    return false;
}

template <typename T>
T field(const BYTE* base, size_t off) {
    T v;
    std::memcpy(&v, base + off, sizeof(T));
    return v;
}

// Reads the live theatre; every access is guarded, so a stale pointer cannot crash the game. No C++ objects here (SEH).
// Result: 0 ok, 1 no theatre loaded, 2 unexpected memory.
int read_raw(HMODULE m, Raw* r) {
    __try {
        const BYTE* obj = static_cast<const BYTE*>(at_rva(m, kRvaTerObject));
        if (field<const void*>(obj, 0) != at_rva(m, kRvaTerVtable)) return 2;
        if (field<DWORD>(obj, kOffLoaded) != 1 || field<DWORD>(obj, kOffKind) != 0) return 1;
        r->samples = field<const uint16_t*>(obj, kOffSamples);
        if (!r->samples) return 1;
        r->w = field<int16_t>(obj, kOffWidth);
        r->h = field<int16_t>(obj, kOffHeight);
        r->spacing = field<double>(obj, kOffSpacing);
        const BYTE* mgr = field<const BYTE*>(obj, kOffManager);
        const BYTE* rec = mgr ? field<const BYTE*>(mgr, kOffCurrent) : nullptr;
        if (!rec) return 1;
        if (!copy_wstr(r->base, field<const wchar_t*>(rec, kOffBaseName)) ||
            !copy_wstr(r->full, field<const wchar_t*>(rec, kOffFullName)))
            return 2;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

bool copy_samples(uint16_t* dst, const uint16_t* src, size_t bytes) {
    __try {
        std::memcpy(dst, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string narrow(const wchar_t* s, size_t max_chars) {
    std::string r;
    for (; *s && r.size() < max_chars; ++s) r += (*s >= 0x20 && *s < 0x7f) ? static_cast<char>(*s) : '?';
    return r;
}

std::atomic<unsigned> g_logged{0};
void log_once(unsigned bit, const char* msg) {
    if (!(g_logged.fetch_or(1u << bit) & (1u << bit))) LOG("%s", msg);
}

std::set<std::string> g_exported;  // game thread only: ids whose files are known to be in place
std::string g_pending_id;          // game thread only: the id of the last job started, not yet settled
std::thread g_thread;
std::atomic<bool> g_busy{false};    // set by the game thread at start, cleared by the thread as its last act
std::atomic<bool> g_job_ok{false};  // the last job's result, stored before g_busy is cleared

/// <summary>Game thread: once the last job has ended, records its id as exported if it succeeded.</summary>
void settle() {
    if (g_pending_id.empty() || g_busy.load(std::memory_order_acquire)) return;
    if (g_job_ok.load(std::memory_order_relaxed)) g_exported.insert(g_pending_id);
    g_pending_id.clear();
}

struct Job {
    std::string id;
    int w, h;
    double spacing;
    std::unique_ptr<uint16_t[]> samples;
};

bool exists_with_size(const std::wstring& p, uint64_t size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    return ((static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow) == size;
}

bool write_all(HANDLE f, const void* data, size_t n) {
    const BYTE* p = static_cast<const BYTE*>(data);
    while (n) {
        const DWORD chunk = n > (1u << 24) ? (1u << 24) : static_cast<DWORD>(n);
        DWORD wrote = 0;
        if (!WriteFile(f, p, chunk, &wrote, nullptr) || wrote != chunk) return false;
        p += chunk;
        n -= chunk;
    }
    return true;
}

/// <summary>Writes body(handle) to path + ".tmp", then moves it over path.</summary>
template <typename Body>
bool write_atomic(const std::wstring& path, Body body) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = body(f);
    ok = CloseHandle(f) && ok;
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// True when the files are in place.
bool export_files(Job& job) {
    PWSTR roaming = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) dir = roaming;
    CoTaskMemFree(roaming);
    if (dir.empty()) {
        LOG("tacview terrain: %s: the AppData folder was not found", job.id.c_str());
        return false;
    }
    dir += L"\\Tacview\\Data\\Terrain\\Custom\\LazySF2";
    const int made = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        LOG("tacview terrain: %s: could not create the folder (error %d)", job.id.c_str(), made);
        return false;
    }
    const std::wstring stem = dir + L"\\" + std::wstring(job.id.begin(), job.id.end());
    const uint64_t raw_bytes = static_cast<uint64_t>(job.w) * static_cast<uint64_t>(job.h) * 2;
    const bool have_raw = exists_with_size(stem + L".raw", raw_bytes);
    // The xml is small: always rewritten, so a changed reference point never leaves stale corners.
    const std::string xml =
        acmi::terrain_xml(job.id, job.id + ".raw", job.w, job.h, job.spacing, kTerrainRefLon, kTerrainRefLat);
    bool ok = true;
    if (!have_raw) {
        // One row at a time, north row first: no second full-size buffer.
        std::vector<uint8_t> row;
        ok = write_atomic(stem + L".raw", [&](HANDLE f) {
            for (int y = job.h - 1; y >= 0; --y) {
                acmi::terrain_raw_north_first(job.samples.get() + static_cast<size_t>(y) * static_cast<size_t>(job.w),
                                              job.w, 1, row);
                if (!write_all(f, row.data(), row.size())) return false;
            }
            return true;
        });
    }
    ok = ok && write_atomic(stem + L".xml", [&](HANDLE f) { return write_all(f, xml.data(), xml.size()); });
    job.samples.reset();
    if (!ok) {
        LOG("tacview terrain: %s: could not write the files (error %lu)", job.id.c_str(), GetLastError());
        return false;
    }
    const std::wstring shown = log::log_path(dir);
    if (have_raw)
        LOG("tacview terrain: %s already present (xml refreshed) in %ls", job.id.c_str(), shown.c_str());
    else
        LOG("tacview terrain: wrote %s (%dx%d at %.0f m, %llu bytes) to %ls", job.id.c_str(), job.w, job.h, job.spacing,
            static_cast<unsigned long long>(raw_bytes), shown.c_str());
    return true;
}

// Stores the result, then clears g_busy as its last act.
void run_job(Job job) {
    bool ok = false;
    try {
        ok = export_files(job);
    } catch (...) {
        log_once(6, "tacview terrain: the export failed with an exception");
    }
    job.samples.reset();
    g_job_ok.store(ok, std::memory_order_relaxed);
    g_busy.store(false, std::memory_order_release);
}

}  // namespace

static bool capture_impl(TerrainInfo& out) {
    static bool build_known = false, build_ok = false;
    HMODULE m = GetModuleHandleW(L"TerrainEngine.dll");
    if (!m) {
        log_once(8, "tacview terrain: TerrainEngine.dll is not loaded; no Theater= for this recording");
        return false;
    }
    if (!build_known) {
        build_known = true;
        build_ok = is_expected_build(m, kStamp, kImageSize, "TerrainEngine.dll");
    }
    if (!build_ok) return false;

    Raw raw;
    const int rc = read_raw(m, &raw);
    if (rc == 2) log_once(0, "tacview terrain: the terrain engine's memory is not as expected; no Theater= for this recording");
    if (rc == 1) log_once(9, "tacview terrain: no theatre is loaded; no Theater= for this recording");
    if (rc != 0) return false;
    if (raw.w < 2 || raw.h < 2 || raw.w > kMaxDim || raw.h > kMaxDim || !std::isfinite(raw.spacing) ||
        raw.spacing < 1.0 || raw.spacing > 100000.0) {
        log_once(1, "tacview terrain: the heightfield size or spacing is out of range; no Theater= for this recording");
        return false;
    }
    out.id = acmi::terrain_id(raw.base);
    if (out.id.empty()) {
        log_once(2, "tacview terrain: the theatre has no name; no Theater= for this recording");
        return false;
    }
    out.full_name = narrow(raw.full, 100);
    out.w = raw.w;
    out.h = raw.h;
    out.spacing = raw.spacing;
    out.samples.reset();
    settle();
    if (g_exported.count(out.id)) return true;
    if (g_busy.load(std::memory_order_acquire)) {
        log_once(10, "tacview terrain: an export is still running; this flight does not start another");
        return true;
    }

    const size_t count = static_cast<size_t>(raw.w) * static_cast<size_t>(raw.h);
    std::unique_ptr<uint16_t[]> buf(new (std::nothrow) uint16_t[count]);
    if (!buf) {
        log_once(3, "tacview terrain: out of memory for the heightmap copy; no export");
        return true;
    }
    if (!copy_samples(buf.get(), raw.samples, count * 2)) {
        log_once(4, "tacview terrain: the heightfield could not be read; no export");
        return true;
    }
    out.samples = std::move(buf);
    return true;
}

bool terrain_capture(TerrainInfo& out) {
    try {
        return capture_impl(out);
    } catch (...) {  // std::bad_alloc from the strings or the set
        out.samples.reset();
        log_once(7, "tacview terrain: out of memory reading the theatre; no Theater= for this recording");
        return false;
    }
}

void terrain_export_start(TerrainInfo& info) {
    if (!info.samples) return;
    if (g_busy.load(std::memory_order_acquire)) return;
    if (g_thread.joinable()) g_thread.join();  // its job has ended (g_busy is clear): this returns at once
    g_job_ok.store(false, std::memory_order_relaxed);
    g_busy.store(true, std::memory_order_release);
    try {
        g_pending_id = info.id;
        g_thread = std::thread(run_job, Job{info.id, info.w, info.h, info.spacing, std::move(info.samples)});
    } catch (...) {  // not marked exported: the next flight may try again
        g_pending_id.clear();
        g_busy.store(false, std::memory_order_release);
        log_once(5, "tacview terrain: could not start the export thread");
    }
}

// Never joins: DllMain holds the loader lock, which an exiting thread needs. The wait is only for g_busy, which the job
// clears before its thread exits, and it is bounded. At process exit the thread is already dead and nothing waits.
void terrain_export_shutdown() {
    if (!g_thread.joinable()) return;
    HANDLE th = static_cast<HANDLE>(g_thread.native_handle());
    if (WaitForSingleObject(th, 0) != WAIT_OBJECT_0) {
        for (int i = 0; i < 100 && g_busy.load(std::memory_order_acquire); ++i) Sleep(10);
    }
    g_thread.detach();
}

}  // namespace lazysf2::hooks
