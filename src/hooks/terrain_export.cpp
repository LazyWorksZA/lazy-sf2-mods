#include "hooks/terrain_export.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "acmi/airfields.h"
#include "acmi/ini.h"
#include "acmi/tacview_terrain.h"
#include "acmi/terrain_texture.h"
#include "hooks/game_files.h"
#include "hooks/hook_util.h"
#include "hooks/wic_jpeg.h"
#include "tacview_addon_lua.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

// TerrainEngine.dll, July 2013 build. See lazy-sf2-recon: recon-notes §19.9 and §21.1.
constexpr DWORD kStamp = 0x51d21fa0, kImageSize = 0x46000;
constexpr DWORD kRvaTerObject = 0x3f650, kRvaTerVtable = 0x3543c;
constexpr size_t kOffKind = 0x08, kOffWidth = 0x78, kOffHeight = 0x7a, kOffSpacing = 0x80, kOffSamples = 0xa4,
                 kOffLon = 0x10, kOffLat = 0x18, kOffLoaded = 0x918, kOffManager = 0x04, kOffCurrent = 0x14,
                 kOffBaseName = 0x00, kOffFullName = 0x04;
// The tile field (TFD), the texture list and the file names. See lazy-sf2-recon: recon-notes §21.1 and §21.4.
constexpr size_t kOffTfdWidth = 0xf0, kOffTfdHeight = 0xf2, kOffTfdRecords = 0x10c, kTfdRecordBytes = 12;
constexpr size_t kOffTexCount = 0x728, kOffTexArray = 0x72c, kTexEntryBytes = 0x71e0, kOffTexColor = 0x12fc,
                 kOffTexUsed = 0x4bd4, kOffTexWater = 0x4bd5;
constexpr size_t kOffDataFile = 0x91c, kOffRecFolder = 0x08, kOffRecDataFile = 0x10, kOffRecTargets = 0x14;
constexpr size_t kNameChars = 260;
constexpr int kMaxDim = 8192;
constexpr int kMaxTiles = 512;      // tiles per side
constexpr int kMaxTextures = 512;
constexpr int kTileToHeightRatio = 4;  // heightmap samples per tile, per side

constexpr int kTexturePixelsPerTile = 8;  // 250 m per pixel at 2000 m tiles: a 4000 x 4000 image for 500 tiles
constexpr float kJpegQuality = 0.9f;

struct Raw {
    wchar_t base[kNameChars + 1];
    wchar_t full[kNameChars + 1];
    int w, h;
    double spacing;
    double lon, lat;
    const uint16_t* samples;
    int tfd_w, tfd_h;
    const BYTE* tfd;
    int tex_count;
    const BYTE* tex;
    bool names_ok;
    wchar_t folder[kNameChars + 1];
    wchar_t targets[kNameChars + 1];
    wchar_t data[kNameChars + 1];
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
        r->lon = field<double>(obj, kOffLon);
        r->lat = field<double>(obj, kOffLat);
        const BYTE* mgr = field<const BYTE*>(obj, kOffManager);
        const BYTE* rec = mgr ? field<const BYTE*>(mgr, kOffCurrent) : nullptr;
        if (!rec) return 1;
        if (!copy_wstr(r->base, field<const wchar_t*>(rec, kOffBaseName)) ||
            !copy_wstr(r->full, field<const wchar_t*>(rec, kOffFullName)))
            return 2;
        // The texture side is optional: a failure here only means no texture and no airfields.
        r->tfd_w = field<int16_t>(obj, kOffTfdWidth);
        r->tfd_h = field<int16_t>(obj, kOffTfdHeight);
        r->tfd = field<const BYTE*>(obj, kOffTfdRecords);
        r->tex_count = field<int>(obj, kOffTexCount);
        r->tex = field<const BYTE*>(obj, kOffTexArray);
        r->names_ok = copy_wstr(r->folder, field<const wchar_t*>(rec, kOffRecFolder)) &&
                      copy_wstr(r->targets, field<const wchar_t*>(rec, kOffRecTargets)) && r->folder[0] && r->targets[0];
        const bool data_in_ter = copy_wstr(r->data, reinterpret_cast<const wchar_t*>(obj + kOffDataFile)) && r->data[0];
        if (!data_in_ter) r->names_ok = r->names_ok && copy_wstr(r->data, field<const wchar_t*>(rec, kOffRecDataFile)) && r->data[0];
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

// Copies the tile field's texture index and rotation. 0 ok, 1 unreadable, 2 a value out of range. No C++ objects (SEH).
int copy_tile_field(const BYTE* tfd, size_t count, int tex_count, uint16_t* index, uint8_t* rot) {
    __try {
        for (size_t i = 0; i < count; ++i) {
            const uint16_t k = field<uint16_t>(tfd, i * kTfdRecordBytes);
            const uint16_t r = field<uint16_t>(tfd, i * kTfdRecordBytes + 2);
            if (k >= tex_count || r > 3) return 2;
            index[i] = k;
            rot[i] = static_cast<uint8_t>(r);
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// 0 ok, 1 unreadable, 2 an entry does not hold its own index at +0 (the layout is not what we think). No C++ objects (SEH).
int copy_texture_entries(const BYTE* tex, int count, TextureEntry* out) {
    __try {
        for (int k = 0; k < count; ++k) {
            const BYTE* e = tex + static_cast<size_t>(k) * kTexEntryBytes;
            if (field<int>(e, 0) != k) return 2;
            for (int c = 0; c < 3; ++c) out[k].color[c] = field<float>(e, kOffTexColor + static_cast<size_t>(c) * 4);
            out[k].used = field<uint8_t>(e, kOffTexUsed) != 0;
            out[k].has_water = field<uint8_t>(e, kOffTexWater) != 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
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

std::mutex g_aero_lock;  // guards g_aero and g_struct
// By theatre id: present once the export thread has read them (or it was decided that none will come).
template <typename T>
using ListMap = std::map<std::string, std::shared_ptr<const T>>;
ListMap<std::vector<AerodromeMark>> g_aero;
ListMap<StructureList> g_struct;

bool list_empty(const std::vector<AerodromeMark>& l) { return l.empty(); }
bool list_empty(const StructureList& l) { return l.marks.empty(); }

// An empty list marks "none"; it never replaces a good non-empty one (a failed retry must not lose it).
template <typename T>
void publish_list(ListMap<T>& map, const std::string& id, T&& list) {
    const bool empty = list_empty(list);
    auto shared = std::make_shared<const T>(std::move(list));  // allocated outside the lock
    std::shared_ptr<const T> old;  // freed after the lock is released
    {
        std::lock_guard<std::mutex> g(g_aero_lock);
        auto& slot = map[id];
        if (slot && empty && !list_empty(*slot)) return;
        old = std::move(slot);
        slot = std::move(shared);
    }
}

template <typename T>
StaticList lookup_list(const ListMap<T>& map, const std::string& id, std::shared_ptr<const T>& out) {
    std::lock_guard<std::mutex> g(g_aero_lock);
    const auto it = map.find(id);
    if (it == map.end() || !it->second) return StaticList::Pending;
    if (list_empty(*it->second)) return StaticList::None;
    out = it->second;
    return StaticList::Ready;
}

// No job will run for this theatre now: the recorder stops waiting. The next flight tries again.
void mark_no_aerodromes(const std::string& id) {
    try {
        publish_list(g_aero, id, std::vector<AerodromeMark>{});
        publish_list(g_struct, id, StructureList{});
    } catch (...) {
    }
}

/// <summary>Game thread: once the last job has ended, records its id as exported if it succeeded.</summary>
void settle() {
    if (g_pending_id.empty() || g_busy.load(std::memory_order_acquire)) return;
    if (g_job_ok.load(std::memory_order_relaxed)) g_exported.insert(g_pending_id);
    g_pending_id.clear();
}

/// <summary>Game thread: the tile field and texture list. Null (logged once) when they are not as expected.</summary>
std::unique_ptr<TextureCapture> capture_texture(const Raw& raw) {
    if (!raw.names_ok) {
        log_once(11, "tacview terrain: the theatre's file names could not be read; no texture or airfields");
        return nullptr;
    }
    try {
        auto t = std::make_unique<TextureCapture>();
        t->folder = raw.folder;
        t->targets_ini = raw.targets;
        t->theatre_ini = std::wstring(raw.base) + L".ini";
        t->data_ini = raw.data;
        const bool sized = raw.tfd_w >= 1 && raw.tfd_h >= 1 && raw.tfd_w <= kMaxTiles && raw.tfd_h <= kMaxTiles &&
                           raw.w == raw.tfd_w * kTileToHeightRatio && raw.h == raw.tfd_h * kTileToHeightRatio;
        if (!sized || !raw.tfd || !raw.tex || raw.tex_count < 1 || raw.tex_count > kMaxTextures) {
            log_once(12, "tacview terrain: the tile field or texture list is not as expected; no texture");
            return t;  // the airfields do not need it
        }
        const size_t tiles = static_cast<size_t>(raw.tfd_w) * static_cast<size_t>(raw.tfd_h);
        std::vector<uint16_t> index(tiles);
        std::vector<uint8_t> rot(tiles);
        std::vector<TextureEntry> entries(static_cast<size_t>(raw.tex_count));
        const int rc = copy_tile_field(raw.tfd, tiles, raw.tex_count, index.data(), rot.data());
        if (rc != 0) {
            log_once(13, rc == 2 ? "tacview terrain: a tile's texture index or rotation is out of range; no texture"
                                 : "tacview terrain: the tile field could not be read; no texture");
            return t;
        }
        const int ec = copy_texture_entries(raw.tex, raw.tex_count, entries.data());
        if (ec != 0) {
            // The texture list is not laid out as expected: no colours, but the tile files can still load by name.
            log_once(16, ec == 2 ? "tacview terrain: the texture list layout is not as expected; tiles without an image are grey"
                                 : "tacview terrain: the texture list could not be read; tiles without an image are grey");
            for (TextureEntry& e : entries) {
                e = TextureEntry{};
                e.color[0] = e.color[1] = e.color[2] = 0.5f;
            }
        }
        t->tiles_w = raw.tfd_w;
        t->tiles_h = raw.tfd_h;
        t->tile_m = raw.spacing * kTileToHeightRatio;
        t->index = std::move(index);
        t->rot = std::move(rot);
        t->entries = std::move(entries);
        return t;
    } catch (...) {
        log_once(14, "tacview terrain: out of memory copying the tile field; no texture or airfields");
        return nullptr;
    }
}

struct Job {
    std::string id;
    int w, h;
    double spacing;
    acmi::Placement place;
    std::unique_ptr<uint16_t[]> samples;
    std::unique_ptr<TextureCapture> texture;
};

bool exists_with_size(const std::wstring& p, uint64_t size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    return ((static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow) == size;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

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

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

/// <summary>Creates dir (and its parents). Logs and returns false on failure.</summary>
bool make_dir(const std::wstring& dir, const std::string& id) {
    const int made = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        LOG("tacview terrain: %s: could not create the folder (error %d)", id.c_str(), made);
        return false;
    }
    return true;
}

// True when the files are in place.
bool export_heightmap(const Job& job, const std::wstring& dir) {
    if (!make_dir(dir, job.id)) return false;
    const std::string file_stem = acmi::terrain_file_stem(job.id);
    const std::wstring stem = dir + L"\\" + std::wstring(file_stem.begin(), file_stem.end());
    const uint64_t raw_bytes = static_cast<uint64_t>(job.w) * static_cast<uint64_t>(job.h) * 2;
    const bool have_raw = exists_with_size(stem + L".raw", raw_bytes);
    // The xml is small: always rewritten, so a changed reference point never leaves stale corners.
    const std::string xml =
        acmi::terrain_xml(job.id, file_stem + ".raw", job.w, job.h, job.spacing,
                          job.place.ref_lon, job.place.ref_lat, job.place.scale_lat);
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

/// <summary>Export thread: the theatre's targets ini; false (logged) when it cannot be read.</summary>
bool read_targets(const Job& job, acmi::Ini& out) {
    const TextureCapture& t = *job.texture;
    std::vector<uint8_t> bytes;
    if (!read_game_file(t.folder + L"\\" + t.targets_ini, bytes)) {
        LOG("tacview terrain: %s: could not read the targets ini; no aerodromes or structures", job.id.c_str());
        return false;
    }
    out = acmi::parse_ini(bytes.data(), bytes.size());
    return true;
}

std::string narrow_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

/// <summary>Export thread: the theatre's airfields from its targets ini.</summary>
std::vector<acmi::Airfield> read_airfields(const Job& job, const acmi::Ini& targets, const acmi::Ini& types) {
    const TextureCapture& t = *job.texture;
    const acmi::AirfieldLoader loader = [&](const std::string& name) {
        std::vector<uint8_t> file;
        if (!read_game_file(t.folder + L"\\" + widen_utf8(name), file)) file.clear();
        return file;
    };
    return acmi::airfields_from(targets, loader, &types);
}

/// <summary>Export thread: the theatre's types ini (named by its theatre ini); empty (logged) when it cannot be read.</summary>
acmi::Ini read_types(const Job& job) {
    const TextureCapture& t = *job.texture;
    std::vector<uint8_t> bytes;
    acmi::Ini theatre;
    if (read_game_file(t.folder + L"\\" + t.theatre_ini, bytes)) theatre = acmi::parse_ini(bytes.data(), bytes.size());
    const std::string types_name = acmi::types_file_name(theatre, narrow_utf8(t.targets_ini));
    if (!types_name.empty() && read_game_file(t.folder + L"\\" + widen_utf8(types_name), bytes))
        return acmi::parse_ini(bytes.data(), bytes.size());
    LOG("tacview terrain: %s: could not read the types ini; structures are named by type id, live types included",
        job.id.c_str());
    return {};
}

/// <summary>
/// Export thread: the theatre's placed structures by target area, on the heightmap and inside it, published for the
/// recorder. Logs the count once per theatre and session.
/// </summary>
void publish_structures(const Job& job, const acmi::Ini& targets, const acmi::Ini& types) {
    acmi::PlacedStructures placed = acmi::structures_from(targets, types);
    const double max_x = (job.w - 1) * job.spacing, max_y = (job.h - 1) * job.spacing;
    StructureList list;
    list.areas = std::move(placed.areas);
    list.first.assign(list.areas.size() + 1, 0);
    list.marks.reserve(placed.items.size());
    size_t next = 0;  // placed.items is grouped by area in area order
    for (size_t a = 0; a < list.areas.size(); ++a) {
        list.first[a] = static_cast<uint32_t>(list.marks.size());
        for (; next < placed.items.size() && placed.items[next].area == a; ++next) {
            const acmi::Structure& s = placed.items[next];
            if (!(s.x >= 0 && s.y >= 0 && s.x <= max_x && s.y <= max_y)) continue;  // off the map
            StructureMark m;
            m.name = s.name;
            m.x = s.x;
            m.y = s.y;
            m.z = acmi::height_at(job.samples.get(), job.w, job.h, job.spacing, s.x, s.y);
            m.heading = s.heading;
            m.bridge = s.bridge;
            list.marks.push_back(std::move(m));
        }
    }
    list.first[list.areas.size()] = static_cast<uint32_t>(list.marks.size());
    LOG("tacview terrain: %s: %zu structures in %zu target areas (%zu more off the map)", job.id.c_str(),
        list.marks.size(), list.areas.size(), placed.items.size() - list.marks.size());
    publish_list(g_struct, job.id, std::move(list));
}

void publish_airfields(const Job& job, const std::vector<acmi::Airfield>& airfields) {
    std::vector<AerodromeMark> list;
    list.reserve(airfields.size());
    for (const acmi::Airfield& a : airfields) {
        AerodromeMark m;
        m.name = a.name;
        m.x = a.x;
        m.y = a.y;
        m.z = acmi::height_at(job.samples.get(), job.w, job.h, job.spacing, a.x, a.y);
        m.heading = a.heading;
        list.push_back(std::move(m));
    }
    publish_list(g_aero, job.id, std::move(list));
}

struct TextureStats {
    int decoded = 0, fallback = 0;
};

/// <summary>One airfield patch to make: its tiles, its xml entry and where its file goes.</summary>
struct PatchPlan {
    size_t airfield = 0;
    acmi::PatchTiles tiles;
    acmi::PatchEntry entry;
    std::wstring path;
    bool present = false;
};

/// <summary>Export thread: the theatre's texture list (index to file name); empty when it cannot be read.</summary>
acmi::Ini read_texture_list(const TextureCapture& t, std::vector<uint8_t>& bytes) {
    return read_game_file(t.folder + L"\\" + t.data_ini, bytes) ? acmi::parse_ini(bytes.data(), bytes.size()) : acmi::Ini{};
}

/// <summary>Export thread: reads and decodes texture k. False when it has no file or cannot be decoded.</summary>
bool decode_texture(const TextureCapture& t, const acmi::Ini& list, size_t k, std::vector<uint8_t>& bytes, acmi::Image& full) {
    char section[16];
    std::snprintf(section, sizeof(section), "Texture%03zu", k + 1);
    const std::string* file = list.get(section, "Filename");
    return file && !file->empty() && read_game_file(t.folder + L"\\" + widen_utf8(*file), bytes) &&
           acmi::decode_image(bytes.data(), bytes.size(), full);
}

/// <summary>Export thread: each texture the tile field uses, as kTexturePixelsPerTile squares (by texture index).</summary>
void load_base_tiles(const TextureCapture& t, std::vector<acmi::Image>& by_index, TextureStats& stats) {
    by_index.assign(t.entries.size(), acmi::Image{});
    std::vector<bool> needed(t.entries.size(), false);
    for (const uint16_t k : t.index)
        if (k < needed.size()) needed[k] = true;
    std::vector<uint8_t> bytes;
    const acmi::Ini list = read_texture_list(t, bytes);
    for (size_t k = 0; k < t.entries.size(); ++k) {
        if (!needed[k]) continue;
        bool done = false;
        try {
            acmi::Image full;
            if (decode_texture(t, list, k, bytes, full)) {
                acmi::box_downsample(full, kTexturePixelsPerTile, by_index[k]);
                done = true;
            }
        } catch (...) {  // out of memory: this tile uses its colour
            by_index[k] = acmi::Image{};
        }
        ++(done ? stats.decoded : stats.fallback);
    }
}

/// <summary>
/// Export thread: re-reads the textures the patches need, each kept at min(native, 512) pixels (halved to fit the
/// cache budget), then all brought to the smallest size. Returns that size.
/// </summary>
int load_patch_tiles(const TextureCapture& t, const std::vector<bool>& want, std::vector<acmi::Image>& by_index) {
    by_index.assign(t.entries.size(), acmi::Image{});
    const size_t patch_count = static_cast<size_t>(std::count(want.begin(), want.end(), true));
    const int cap = acmi::patch_tile_px(acmi::kPatchTileMaxPx, patch_count, acmi::kPatchCacheBytes);
    std::vector<uint8_t> bytes;
    const acmi::Ini list = read_texture_list(t, bytes);
    for (size_t k = 0; k < t.entries.size(); ++k) {
        if (!want[k]) continue;
        try {
            acmi::Image full;
            if (decode_texture(t, list, k, bytes, full))
                acmi::box_downsample(full, std::min({full.w, full.h, cap}), by_index[k]);
        } catch (...) {  // out of memory: this tile uses its colour
            by_index[k] = acmi::Image{};
        }
    }
    return acmi::unify_tile_size(by_index, cap);
}

std::vector<acmi::RGB> entry_colours(const TextureCapture& t) {
    std::vector<acmi::RGB> colours(t.entries.size());
    const auto to_byte = [](float c) {
        return static_cast<uint8_t>(std::isfinite(c) ? std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f) : 128);
    };
    for (size_t k = 0; k < colours.size(); ++k)
        colours[k] = {to_byte(t.entries[k].color[0]), to_byte(t.entries[k].color[1]), to_byte(t.entries[k].color[2])};
    return colours;
}

/// <summary>Writes img as a JPEG through a temporary name. False (and no file) on failure.</summary>
bool write_jpeg_file(const acmi::RGBImage& img, const std::wstring& path) {
    // The encoder opens the file itself: temporary name first, then moved into place.
    const std::wstring tmp = path + L".tmp";
    bool ok = write_jpeg_wic(img, tmp, kJpegQuality);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

/// <summary>Composes one patch from the cached tiles, shrinks it to fit and paints the runways.</summary>
void build_patch(const TextureCapture& t, const PatchPlan& p, const acmi::Airfield& a, const std::vector<acmi::Image>& cache,
                 int cache_px, const std::vector<acmi::RGB>& colours, acmi::RGBImage& img) {
    const acmi::PatchTiles& pt = p.tiles;
    // A big patch is composed from smaller tiles, so it never allocates more than the size cap.
    const int px = acmi::patch_compose_px(cache_px, pt.nx, pt.ny);
    std::vector<acmi::Image> reduced;
    const std::vector<acmi::Image>* use = &cache;
    if (px != cache_px) {
        reduced.assign(cache.size(), acmi::Image{});
        for (int y = 0; y < pt.ny; ++y)
            for (int x = 0; x < pt.nx; ++x) {
                const size_t k = t.index[static_cast<size_t>(pt.ty0 + y) * static_cast<size_t>(t.tiles_w) +
                                         static_cast<size_t>(pt.tx0 + x)];
                if (k < cache.size() && cache[k].w == cache_px && reduced[k].w == 0) acmi::box_downsample(cache[k], px, reduced[k]);
            }
        use = &reduced;
    }
    acmi::compose_patch(pt, t.tiles_w, px, t.index.data(), t.rot.data(), *use, colours, img);
    int w = img.w, h = img.h;
    acmi::fit_patch_size(img.w, img.h, w, h);
    if (w != img.w || h != img.h) {
        acmi::RGBImage shrunk;
        acmi::resize_box_rgb(img, w, h, shrunk);
        img = std::move(shrunk);
    }
    acmi::draw_runways(img, a, pt.x0, pt.y0, pt.w_m, pt.h_m);
}

/// <summary>Export thread: the ground texture, its xml and the airfield patches. COM is initialised on this thread.</summary>
void export_texture(const Job& job, const std::vector<acmi::Airfield>& airfields, const std::wstring& dir) {
    const TextureCapture& t = *job.texture;
    if (t.tiles_w == 0) return;  // already logged at capture
    if (!make_dir(dir, job.id)) return;
    const std::string file_stem = acmi::terrain_file_stem(job.id);
    const std::wstring stem = dir + L"\\" + std::wstring(file_stem.begin(), file_stem.end());
    const std::wstring shown = log::log_path(dir);
    const int n = kTexturePixelsPerTile;
    const double extent_x = t.tiles_w * t.tile_m, extent_y = t.tiles_h * t.tile_m;
    const bool have_jpg = exists(stem + L".jpg");

    std::vector<PatchPlan> plans;
    std::vector<bool> want_patch(t.entries.size(), false);
    try {
        char suffix[16];
        for (size_t i = 0; i < airfields.size(); ++i) {
            PatchPlan p;
            p.airfield = i;
            if (!acmi::patch_tiles(airfields[i], t.tile_m, t.tiles_w, t.tiles_h, p.tiles)) continue;
            p.entry.x0 = p.tiles.x0;
            p.entry.y0 = p.tiles.y0;
            p.entry.w_m = p.tiles.w_m;
            p.entry.h_m = p.tiles.h_m;
            std::snprintf(suffix, sizeof suffix, ".AF%03zu", i + 1);
            p.entry.id = job.id + suffix;
            std::snprintf(suffix, sizeof suffix, "-AF%03zu", i + 1);
            p.entry.file = file_stem + suffix + ".jpg";
            p.path = dir + L"\\" + std::wstring(p.entry.file.begin(), p.entry.file.end());
            p.present = exists(p.path);
            if (!p.present)
                for (int y = 0; y < p.tiles.ny; ++y)
                    for (int x = 0; x < p.tiles.nx; ++x) {
                        const size_t k = t.index[static_cast<size_t>(p.tiles.ty0 + y) * static_cast<size_t>(t.tiles_w) +
                                                 static_cast<size_t>(p.tiles.tx0 + x)];
                        if (k < want_patch.size()) want_patch[k] = true;
                    }
            plans.push_back(std::move(p));
        }
    } catch (...) {
        log_once(19, "tacview terrain: airfield patches stopped: out of memory");
        plans.clear();
    }
    const bool patches_to_write =
        std::any_of(plans.begin(), plans.end(), [](const PatchPlan& p) { return !p.present; });

    const std::vector<acmi::RGB> colours = entry_colours(t);
    // Phase 1, the base texture: its tiles, image and encoder buffers are gone before the patch cache is built.
    if (!have_jpg) {
        TextureStats stats;
        try {
            std::vector<acmi::Image> base;
            load_base_tiles(t, base, stats);
            acmi::RGBImage img;
            acmi::compose(t.tiles_w, t.tiles_h, n, t.index.data(), t.rot.data(), base, colours, img);
            base = {};
            if (img.px.empty()) {
                LOG("tacview terrain: %s: could not compose the texture", job.id.c_str());
                return;
            }
            const double mpp = t.tile_m / n;
            for (const acmi::Airfield& a : airfields)
                for (const acmi::Runway& r : a.runways) acmi::paint_runway(img, mpp, r.x, r.y, r.heading, r.length, r.width);
            if (!write_jpeg_file(img, stem + L".jpg")) {
                LOG("tacview terrain: %s: could not encode the texture; there will be none", job.id.c_str());
                return;
            }
            LOG("tacview terrain: wrote the %s texture (%dx%d, %d tiles' images decoded, %d from colour, %zu airfields) to %ls",
                job.id.c_str(), img.w, img.h, stats.decoded, stats.fallback, airfields.size(), shown.c_str());
        } catch (...) {
            log_once(15, "tacview terrain: no texture: out of memory");
            return;
        }
    } else {
        LOG("tacview terrain: the %s texture is already present in %ls", job.id.c_str(), shown.c_str());
    }

    // Phase 2, the patches. They depend only on the airfields, so they are made whether or not the base texture was.
    std::vector<acmi::PatchEntry> patches;
    int written = 0, present = 0, failed = 0, tile_px = 0;
    std::vector<acmi::Image> cache;
    bool cache_ok = true;
    if (patches_to_write) {
        try {
            tile_px = load_patch_tiles(t, want_patch, cache);
        } catch (...) {
            cache_ok = false;
            log_once(20, "tacview terrain: airfield patches stopped: out of memory");
        }
    }
    for (PatchPlan& p : plans) {
        if (p.present) {
            ++present;
        } else {
            bool ok = false;
            if (cache_ok) {
                try {
                    acmi::RGBImage img;
                    build_patch(t, p, airfields[p.airfield], cache, tile_px, colours, img);
                    ok = !img.px.empty() && write_jpeg_file(img, p.path);
                } catch (...) {
                }
            }
            if (!ok) {
                ++failed;
                LOG("tacview terrain: %s: could not write the airfield patch for airfield %zu; skipped", job.id.c_str(),
                    p.airfield + 1);
                continue;
            }
            ++written;
        }
        patches.push_back(std::move(p.entry));
    }
    cache = {};
    if (written + present + failed > 0)
        LOG("tacview terrain: %s: %d airfield patches (%d written, %d already present, %d failed), %d px tiles", job.id.c_str(),
            written + present, written, present, failed, tile_px);
    // The xml is small: always rewritten, so a changed reference point never leaves stale corners.
    const std::string xml = acmi::texture_xml(job.id, file_stem + ".jpg", t.tiles_w * n, t.tiles_h * n, extent_x, extent_y,
                                              job.place.ref_lon, job.place.ref_lat, job.place.scale_lat,
                                              patches);
    if (!write_atomic(stem + L".xml", [&](HANDLE f) { return write_all(f, xml.data(), xml.size()); }))
        LOG("tacview terrain: %s: could not write the texture xml (error %lu)", job.id.c_str(), GetLastError());
}

/// <summary>Deletes the files earlier versions wrote for this theatre (dotted names Tacview cannot load, png patches). Returns the count removed.</summary>
int remove_legacy_files(const std::wstring& dir, const std::string& id) {
    const std::wstring wid(id.begin(), id.end());
    const std::wstring base = dir + L"\\" + wid;
    int removed = 0;
    for (const wchar_t* ext : {L".raw", L".jpg", L".xml", L".raw.tmp", L".jpg.tmp", L".xml.tmp"})
        if (DeleteFileW((base + ext).c_str())) ++removed;
    std::vector<std::wstring> found;
    // The dotted names, and the dash names of the earlier transparent png patches: exact patterns, our folder only.
    const std::string file_stem = acmi::terrain_file_stem(id);
    const std::wstring dash_stem(file_stem.begin(), file_stem.end());
    const std::pair<std::wstring, std::wstring> patterns[] = {{wid + L".AF", L".png"}, {dash_stem + L"-AF", L".png"}};
    for (const auto& [prefix, ext] : patterns) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + prefix + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = fd.cFileName;
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            size_t i = prefix.size();
            while (i < name.size() && name[i] >= L'0' && name[i] <= L'9') ++i;
            const std::wstring rest = name.substr(i);
            if (i > prefix.size() && (rest == ext || rest == ext + L".tmp")) found.push_back(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const std::wstring& name : found)
        if (DeleteFileW((dir + L"\\" + name).c_str())) ++removed;
    return removed;
}

// Writes the embedded Tacview add-on to %APPDATA%\Tacview\AddOns\LazySF2Patch\main.lua, only when the content differs.
// Once per session: the export thread runs one job at a time, so a plain static is enough.
void install_addon(const std::wstring& appdata) {
    static bool done = false;
    if (done) return;
    const std::string_view text(kTacviewAddonLua, sizeof(kTacviewAddonLua) - 1);
    std::wstring dir = appdata + L"\\Tacview";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\AddOns";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\LazySF2Patch";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\main.lua", tmp = path + L".tmp";
    std::string current;
    if (HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart == static_cast<LONGLONG>(text.size())) {
            current.resize(text.size());
            DWORD got = 0;
            if (!ReadFile(h, current.data(), static_cast<DWORD>(current.size()), &got, nullptr) || got != current.size()) current.clear();
        }
        CloseHandle(h);
    }
    done = true;  // one attempt per session: a failure is logged once, not retried every flight
    if (current == text) return;
    bool ok = false;
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
        CloseHandle(h);
        ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok) DeleteFileW(tmp.c_str());
    }
    if (ok) log_once(21, "tacview terrain: wrote the Tacview add-on (AddOns\\LazySF2Patch\\main.lua)");
    else log_once(22, "tacview terrain: could not write the Tacview add-on");
}

// True when the heightmap is in place. The texture and the airfields are best-effort.
bool export_files(Job& job) {
    PWSTR roaming = nullptr;
    std::wstring root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) root = roaming;
    CoTaskMemFree(roaming);
    if (root.empty()) {
        LOG("tacview terrain: %s: the AppData folder was not found", job.id.c_str());
        return false;
    }
    install_addon(root);
    root += L"\\Tacview\\Data\\Terrain";
    const int removed = remove_legacy_files(root + L"\\Custom\\LazySF2Patch", job.id) +
                        remove_legacy_files(root + L"\\Textures\\LazySF2Patch", job.id);
    if (removed > 0) LOG("tacview terrain: %s: removed %d old file(s) (dotted names Tacview could not load, or old png patches)", job.id.c_str(), removed);
    const bool ok = export_heightmap(job, root + L"\\Custom\\LazySF2Patch");
    if (!job.texture) return ok;
    try {
        acmi::Ini targets;
        std::vector<acmi::Airfield> airfields;
        if (read_targets(job, targets)) {
            const acmi::Ini types = read_types(job);
            airfields = read_airfields(job, targets, types);
            publish_airfields(job, airfields);  // before the slow part: a recording started now gets its aerodromes
            publish_structures(job, targets, types);
        }
        export_texture(job, airfields, root + L"\\Textures\\LazySF2Patch");
    } catch (...) {  // the heightmap is done: only the texture and the airfields are lost
        log_once(18, "tacview terrain: no texture: out of memory");
    }
    return ok;
}

// Stores the result, then clears g_busy as its last act.
void run_job(Job job) {
    // WIC needs COM on this thread; every COM object is released inside export_files, before CoUninitialize.
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    bool ok = false;
    try {
        ok = export_files(job);
    } catch (...) {
        log_once(6, "tacview terrain: the export failed with an exception");
    }
    if (com) CoUninitialize();
    mark_no_aerodromes(job.id);  // a recording must never wait for a list that will not come
    job.samples.reset();
    job.texture.reset();
    g_job_ok.store(ok, std::memory_order_relaxed);
    g_busy.store(false, std::memory_order_release);
}

}  // namespace

static bool capture_impl(TerrainInfo& out, bool basics_only) {
    static bool build_known = false, build_ok = false;
    // The snapshot probe must not use up the recorder's once-only lines.
    auto note = [basics_only](unsigned bit, const char* msg) {
        if (!basics_only) log_once(bit, msg);
    };
    HMODULE m = GetModuleHandleW(L"TerrainEngine.dll");
    if (!m) {
        note(8, "tacview terrain: TerrainEngine.dll is not loaded; no Theater= for this recording");
        return false;
    }
    if (!build_known) {
        build_known = true;
        build_ok = is_expected_build(m, kStamp, kImageSize, "TerrainEngine.dll");
    }
    if (!build_ok) return false;

    Raw raw;
    const int rc = read_raw(m, &raw);
    if (rc == 2) note(0, "tacview terrain: the terrain engine's memory is not as expected; no Theater= for this recording");
    if (rc == 1) note(9, "tacview terrain: no theatre is loaded; no Theater= for this recording");
    if (rc != 0) return false;
    if (raw.w < 2 || raw.h < 2 || raw.w > kMaxDim || raw.h > kMaxDim || !std::isfinite(raw.spacing) ||
        raw.spacing < 1.0 || raw.spacing > 100000.0) {
        note(1, "tacview terrain: the heightfield size or spacing is out of range; no Theater= for this recording");
        return false;
    }
    out.id = acmi::terrain_id(raw.base);
    if (out.id.empty()) {
        note(2, "tacview terrain: the theatre has no name; no Theater= for this recording");
        return false;
    }
    out.full_name = narrow(raw.full, 100);
    out.w = raw.w;
    out.h = raw.h;
    out.spacing = raw.spacing;
    out.has_geo = acmi::geo_is_set(raw.lon, raw.lat);
    out.geo_lon = raw.lon;
    out.geo_lat = raw.lat;
    out.samples.reset();
    out.texture.reset();
    if (basics_only) return true;
    settle();
    if (g_exported.count(out.id)) return true;
    if (g_busy.load(std::memory_order_acquire)) {
        note(10, "tacview terrain: an export is still running; this flight does not start another");
        if (g_pending_id != out.id) mark_no_aerodromes(out.id);  // same theatre: its running job publishes the list
        return true;
    }

    const size_t count = static_cast<size_t>(raw.w) * static_cast<size_t>(raw.h);
    std::unique_ptr<uint16_t[]> buf(new (std::nothrow) uint16_t[count]);
    if (!buf) {
        note(3, "tacview terrain: out of memory for the heightmap copy; no export");
        mark_no_aerodromes(out.id);
        return true;
    }
    if (!copy_samples(buf.get(), raw.samples, count * 2)) {
        note(4, "tacview terrain: the heightfield could not be read; no export");
        mark_no_aerodromes(out.id);
        return true;
    }
    out.samples = std::move(buf);
    out.texture = capture_texture(raw);
    return true;
}

bool terrain_capture(TerrainInfo& out) {
    try {
        return capture_impl(out, false);
    } catch (...) {  // std::bad_alloc from the strings or the set
        out.samples.reset();
        out.texture.reset();
        log_once(7, "tacview terrain: out of memory reading the theatre; no Theater= for this recording");
        return false;
    }
}

bool terrain_basics(TerrainInfo& out) {
    try {
        return capture_impl(out, true);
    } catch (...) {
        out.samples.reset();
        out.texture.reset();
        return false;
    }
}

void terrain_export_start(TerrainInfo& info, const acmi::Placement& placement) {
    if (!info.samples) return;
    if (g_busy.load(std::memory_order_acquire)) {
        if (g_pending_id != info.id) mark_no_aerodromes(info.id);  // same theatre: its running job publishes the list
        return;
    }
    if (g_thread.joinable()) g_thread.join();  // its job has ended (g_busy is clear): this returns at once
    g_job_ok.store(false, std::memory_order_relaxed);
    g_busy.store(true, std::memory_order_release);
    try {
        g_pending_id = info.id;
        g_thread = std::thread(run_job,
                               Job{info.id, info.w, info.h, info.spacing, placement, std::move(info.samples), std::move(info.texture)});
    } catch (...) {  // not marked exported: the next flight may try again
        g_pending_id.clear();
        g_busy.store(false, std::memory_order_release);
        mark_no_aerodromes(info.id);
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

StaticList terrain_aerodromes(const std::string& id, std::shared_ptr<const std::vector<AerodromeMark>>& out) {
    return lookup_list(g_aero, id, out);
}

StaticList terrain_structures(const std::string& id, std::shared_ptr<const StructureList>& out) {
    return lookup_list(g_struct, id, out);
}

}  // namespace LazyWorks::SF2Patch::hooks
