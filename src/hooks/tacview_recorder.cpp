// Tacview recorder (player-facing): with Options > Other > Tacview Recording On, writes one recording per flight (.acmi, packed to .zip.acmi afterwards).
// Inert while Off (no hooks, thread, memory or file). Game thread only copies into a ring: no allocation, I/O or logging.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "acmi/tacview_core.h"
#include "acmi/acmi_zip.h"
#include "acmi/tacview_writer.h"
#include "config.h"
#include "hooks/hooks.h"
#include "hooks/mission_areas.h"
#include "hooks/terrain_export.h"
#include "hooks/weapon_class.h"
#include "hooks/world_walk.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr size_t kOffPos = 0x38, kOffAngles = 0x100;  // aircraft object: double[3] position, float[3] angles
// Weapon object (MissileObject): position double[3] at +0x20; angles float[3] (pitch, heading, roll) at +0xe8.
constexpr size_t kOffWeaponPos = 0x20, kOffWeaponAngles = 0xe8;
// Every object: int, 0 once it has finished; its memory stays valid. See lazy-sf2-recon: recon-notes §20.1.3.
constexpr size_t kOffActive = 0x04;
// Locks. Aircraft radar block at +0x970: int state (+0x970; 2 search, 3 and 4 track, 5 ground), int stage (+0x974;
// 2 tracking, 3 locked) and the target's object pointer (+0x988). Missile: target object pointer at +0x194 (kept only
// while the target is active). Read only from objects the build-checked trackers hold.
// See lazy-sf2-recon: recon-notes §27.
constexpr size_t kOffRadarState = 0x970, kOffRadarStage = 0x974, kOffRadarTarget = 0x988;
constexpr size_t kOffWeaponTarget = 0x194;
constexpr double kSnapshotInterval = 0.1;        // game seconds between snapshots: 10 Hz (aircraft only)
constexpr double kSnapshotIntervalWeapons = 0.05;  // 20 Hz while a weapon is in flight
constexpr size_t kMaxAerodromes = 512;
// Structures go out in chunks of their own, a few steps apart, so a theatre's thousands never fill the ring at once.
constexpr size_t kMaxStructures = 32768;
constexpr size_t kStructuresPerFrame = 512;
// They wait for the first world walk that listed the ground objects, so the core can leave out those the game made live.
// Without one by then (no ground group), they go out anyway.
constexpr double kStructureWaitSeconds = 5.0;
// A chunk goes out only while the ring could still take a full world walk and an aircraft snapshot after it.
constexpr size_t kStructureHeadroom = kWorldWalkMaxRecords + 256;
// Only the areas the mission populated are recorded. Without the game's own set (not a single mission): the areas
// within their radius plus this of the player's first position, and those with a live ground object inside them.
constexpr double kFallbackRangeMetres = 30000.0;
constexpr size_t kMaxMissionAreas = 4096;  // the game's set, and the theatre's areas we can pick from
constexpr size_t kMaxPlacedPerArea = 4096;
// A mark and the game's placed object are the same when this close (the game sums in float).
constexpr double kPlacedMatchMetres = 2.0;
// Placed structures the game drew are checked for destruction this often (game seconds), at most this many per frame.
constexpr double kDestroyedPollSeconds = 1.0;
constexpr size_t kMaxDestroyedPerFrame = 256;

static_assert(static_cast<uint8_t>(WeaponClass::Missile) == acmi::TacviewRec::kMissile &&
                  static_cast<uint8_t>(WeaponClass::Bomb) == acmi::TacviewRec::kBomb &&
                  static_cast<uint8_t>(WeaponClass::Rocket) == acmi::TacviewRec::kRocket,
              "weapon classes must match the recorder's");

acmi::TacviewWriter* g_writer = nullptr;  // allocated at the first recording (game thread)
std::wstring g_dir;                       // ...\Tacview, resolved once
bool g_dir_tried = false;
bool g_recording = false;
uint32_t g_epoch = 0;        // sim_player_epoch() at the start
std::atomic<DWORD> g_thread{0};  // the game thread that started the recording (read by the tracker on any thread)
bool g_skip_valid = false;   // a start failed or the writer failed during this player's flight: do not retry it
uint32_t g_skip_epoch = 0;
double g_last_snapshot = 0;
std::unique_ptr<acmi::TacviewRec[]> g_walk_buf;  // the world walker's records, copied into the ring as one frame
uint64_t g_dead_sent[64];  // aircraft seqs whose Destroyed marker was sent this recording
size_t g_dead_n = 0;
uint64_t g_wdead_sent[256];  // the same for weapons (a repeat is harmless: the core ignores it)
size_t g_wdead_n = 0;
bool g_walk_on = false;                          // the walker started with this recording
std::string g_theater;       // the theatre id of this recording; empty when the terrain was not captured
bool g_aero_pending = false;  // this recording's aerodromes are not written yet
bool g_struct_pending = false;  // this recording's structures are not all written yet
std::shared_ptr<const StructureList> g_struct_list;  // held while the chunks go out (the map keeps it too)
bool g_struct_picked = false;    // g_mark_write is filled for g_struct_list
size_t g_struct_mark = 0;        // the next mark to look at
size_t g_struct_written = 0;
bool g_ground_walked = false;    // a world frame that lists the ground objects is in the ring
double g_struct_wait_from = 0;   // game time of this recording's first step; NaN before it
// Fixed buffers, so the game thread never allocates for them.
GameArea g_game_areas[kMaxMissionAreas];  // the single mission's populated areas, read at the start
double g_active_xy[kMaxMissionAreas][2];
size_t g_active_n = 0;
MissionAreas g_active_src = MissionAreas::NotSingle;
double g_ground_xy[kWorldWalkMaxRecords][2];  // the first ground walk's GroundObject positions (for the fallback)
size_t g_ground_n = 0;
bool g_player_known = false;
double g_player_xy[2] = {};
uint8_t g_area_on[kMaxMissionAreas];
uint8_t g_mark_write[kMaxStructures];  // per mark of the theatre: write it
PlacedView g_placed[kMaxPlacedPerArea];
double g_placed_xy[kMaxPlacedPerArea][2];
uint8_t g_placed_taken[kMaxPlacedPerArea];  // claimed by a mark: one record matches one mark at most
// The written structures the game drew as plain models: their records, polled for destruction.
const void* g_watch_rec[kMaxStructures];
uint32_t g_watch_seq[kMaxStructures];
uint8_t g_watch_dead[kMaxStructures];
size_t g_watch_n = 0;
uint32_t g_watch_hits[kMaxDestroyedPerFrame];
double g_last_poll = 0;
// For the developer destroy hotkey only: where each watched record stands, and which area and placed index it is.
double g_watch_xy[kMaxStructures][2];
uint16_t g_watch_area[kMaxStructures];
uint16_t g_watch_index[kMaxStructures];
uint8_t g_destroy_skip[kMaxStructures];
int g_placed_index[kMaxPlacedPerArea];  // the game's index of g_placed[k]
std::atomic<bool> g_destroy_request{false};  // set by the probe hotkey thread
bool g_weapons_live = false;  // the last snapshot held a weapon: snapshot at the faster rate
std::atomic<bool> g_registered{false};
std::atomic<bool> g_table_empty{false};  // set by the tracker when the table empties; cleared at a start

void writer_log(const char* line) { LOG("%s", line); }

std::wstring display_path(const std::wstring& p) { return log::log_path(p); }

// Reads the pose of a live aircraft; false instead of a crash if the memory went away. No C++ objects here (SEH).
bool read_pose(const void* obj, size_t off_pos, size_t off_ang, double pos[3], float ang[3]) {
    __try {
        const char* b = static_cast<const char*>(obj);
        std::memcpy(pos, b + off_pos, 3 * sizeof(double));
        std::memcpy(ang, b + off_ang, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// An object's active flag. True when unreadable, so that the pose read decides. No C++ objects (SEH).
bool read_active(const void* obj) {
    __try {
        return *reinterpret_cast<const volatile int*>(static_cast<const char*>(obj) + kOffActive) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// The aircraft's radar target while it tracks or locks an air target, else null. No C++ objects (SEH).
const void* read_radar_target(const void* obj) {
    __try {
        const char* b = static_cast<const char*>(obj);
        const int state = *reinterpret_cast<const volatile int*>(b + kOffRadarState);
        const int stage = *reinterpret_cast<const volatile int*>(b + kOffRadarStage);
        if (state < 2 || state > 4 || stage < 2) return nullptr;
        return *reinterpret_cast<const void* const volatile*>(b + kOffRadarTarget);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// A guided missile's target object, or null. No C++ objects (SEH).
const void* read_weapon_target(const void* obj) {
    __try {
        return *reinterpret_cast<const void* const volatile*>(static_cast<const char*>(obj) + kOffWeaponTarget);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

/// <summary>Sets r's lock to the listed aircraft at target (by pointer); none when null or not listed.</summary>
void set_lock(acmi::TacviewRec& r, const void* target, const LiveAircraft* list, size_t n) {
    r.lock_mode = 0;
    r.lock_seq = 0;
    if (!target) return;
    for (size_t k = 0; k < n; ++k) {
        if (list[k].obj == target) {
            r.lock_mode = 1;
            r.lock_seq = static_cast<uint32_t>(list[k].seq);
            return;
        }
    }
}

struct SnapCtx {
    double t = 0;
    bool wrote = false;
    const LiveAircraft* air = nullptr;  // valid inside the aircraft callback, where the weapon callback runs
    size_t n_air = 0;
};

void copy_name(char* dst, const wchar_t* src) {
    size_t k = 0;
    for (; k < acmi::kRecNameChars && src[k]; ++k) {
        const wchar_t ch = src[k];
        dst[k] = (ch >= 0x20 && ch < 0x7f) ? static_cast<char>(ch) : '?';
    }
    dst[k] = '\0';
}

// Under both tables' shared locks (the weapons' nested in the aircraft's): no allocation, I/O or logging.
// Writes the whole frame, aircraft first (a weapon's parent must be known to the core before the weapon).
void weapon_cb(const LiveWeapon* wl, size_t nw, void* vctx) {
    auto& c = *static_cast<SnapCtx*>(vctx);
    const LiveAircraft* list = c.air;
    const size_t n = c.n_air;
    if (!g_writer->begin_frame(n + nw)) return;
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        acmi::TacviewRec& r = g_writer->slot(used);
        r.obj_class = acmi::TacviewRec::kAircraft;
        r.parent_seq = 0;
        r.destroyed = 0;
        r.lock_mode = 0;  // ring slots are reused
        r.lock_seq = 0;
        if (!read_active(list[i].obj)) {
            // Finished (destroyed): left out of the frame, so the core removes it; the marker adds the Destroyed event,
            // once per aircraft (a full list just repeats it, which the core ignores).
            bool sent = false;
            for (size_t k = 0; k < g_dead_n && !sent; ++k) sent = g_dead_sent[k] == list[i].seq;
            if (sent) continue;
            if (g_dead_n < sizeof(g_dead_sent) / sizeof(g_dead_sent[0])) g_dead_sent[g_dead_n++] = list[i].seq;
            r.kind = acmi::TacviewRec::kObject;
            r.destroyed = acmi::TacviewRec::kDestroyed;
            r.is_player = list[i].is_player ? 1 : 0;
            r.seq = list[i].seq;
            r.t = c.t;
            ++used;
            continue;
        }
        if (!read_pose(list[i].obj, kOffPos, kOffAngles, r.pos, r.ang)) {
            // Keep the object in the frame with a non-finite pose: the core counts it as seen (no remove and re-add)
            // and skips the transform.
            r.kind = acmi::TacviewRec::kObject;
            r.seq = list[i].seq;
            r.t = c.t;
            r.pos[0] = r.pos[1] = r.pos[2] = std::nan("");
            ++used;
            continue;
        }
        r.kind = acmi::TacviewRec::kObject;
        r.side = static_cast<uint8_t>(list[i].side);
        r.is_player = list[i].is_player ? 1 : 0;
        r.seq = list[i].seq;
        r.t = c.t;
        copy_name(r.name, list[i].name);
        size_t t = 0;
        for (; t < acmi::kRecTypeChars && list[i].type_name[t]; ++t) r.type[t] = list[i].type_name[t];
        r.type[t] = '\0';
        set_lock(r, read_radar_target(list[i].obj), list, n);
        ++used;
    }
    size_t live_weapons = 0;
    for (size_t i = 0; i < nw; ++i) {
        if (!read_active(wl[i].obj)) {
            // Detonated or burnt out: the core removes it and writes an explosion (no Destroyed event), once.
            bool sent = false;
            for (size_t k = 0; k < g_wdead_n && !sent; ++k) sent = g_wdead_sent[k] == wl[i].seq;
            if (sent) continue;
            if (g_wdead_n < sizeof(g_wdead_sent) / sizeof(g_wdead_sent[0])) g_wdead_sent[g_wdead_n++] = wl[i].seq;
            acmi::TacviewRec& m = g_writer->slot(used);
            m.lock_mode = 0;
            m.lock_seq = 0;
            m.kind = acmi::TacviewRec::kObject;
            m.obj_class = wl[i].weapon_class;
            m.destroyed = acmi::TacviewRec::kDetonated;
            m.seq = wl[i].seq;
            m.t = c.t;
            ++used;
            continue;
        }
        ++live_weapons;
        acmi::TacviewRec& r = g_writer->slot(used);
        r.obj_class = wl[i].weapon_class;
        r.destroyed = 0;
        r.lock_mode = 0;
        r.lock_seq = 0;
        r.parent_class = acmi::TacviewRec::kAircraft;  // ring slots are reused
        r.kind = acmi::TacviewRec::kObject;
        r.is_player = 0;
        r.type[0] = '\0';  // weapons have no type field (ring slots are reused)
        r.seq = wl[i].seq;
        r.parent_seq = wl[i].parent_seq;
        r.t = c.t;
        if (!read_pose(wl[i].obj, kOffWeaponPos, kOffWeaponAngles, r.pos, r.ang)) {
            r.pos[0] = r.pos[1] = r.pos[2] = std::nan("");  // seen, but no transform (see above)
            r.side = 0;
            r.name[0] = '\0';
            ++used;
            continue;
        }
        r.side = static_cast<uint8_t>(wl[i].side);
        size_t k = 0;
        for (; k < acmi::kRecNameChars && wl[i].name[k]; ++k) r.name[k] = wl[i].name[k];
        r.name[k] = '\0';
        if (wl[i].weapon_class == static_cast<uint8_t>(WeaponClass::Missile))
            set_lock(r, read_weapon_target(wl[i].obj), list, n);
        ++used;
    }
    g_weapons_live = live_weapons != 0;
    g_writer->commit_frame(used, c.t, acmi::TacviewRec::kSeriesAircraft | acmi::TacviewRec::kSeriesWeapon);
    c.wrote = true;
}

// Under the aircraft table's shared lock.
void snapshot_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<SnapCtx*>(vctx);
    if (n == 0) return;
    c.air = list;
    c.n_air = n;
    probe_with_weapons(weapon_cb, &c);
}

// One last frame with no objects, so the file gets a removal line for every object (the player's -1 included).
void stop_recording(const char* why, bool final_frame = false) {
    if (!g_recording) return;
    g_recording = false;
    if (g_walk_on) world_walk_stop();
    g_walk_on = false;
    g_struct_pending = false;
    g_struct_list.reset();  // never the last owner: the export's map keeps the list
    g_watch_n = 0;          // the mission's records are not read after the recording
    if (final_frame && g_writer->accepting() && g_writer->begin_frame(0)) g_writer->commit_frame(0, sim_game_time());
    g_writer->close(false, why);  // the thread logs the summary
}

enum class StartResult { Started, Failed, Retry };

StartResult start_recording(uint32_t epoch) {
    if (!g_dir_tried) {
        g_dir_tried = true;
        const std::wstring base = config::game_data_dir();  // the shell: game thread, once
        if (!base.empty()) g_dir = base + L"\\Tacview";
    }
    if (g_dir.empty()) {
        LOG("tacview: not recording: the Saved Games folder was not found");
        return StartResult::Failed;
    }
    if (!g_writer) g_writer = new (std::nothrow) acmi::TacviewWriter;
    if (!g_writer || !g_writer->ready()) {
        LOG("tacview: not recording: out of memory for the frame ring");
        return StartResult::Failed;
    }
    if (!g_writer->idle()) return StartResult::Retry;  // the previous recording is still being written out
    SYSTEMTIME local, utc;
    GetLocalTime(&local);
    GetSystemTime(&utc);
    char iso[40];
    std::snprintf(iso, sizeof(iso), "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                  utc.wMinute, utc.wSecond);
    acmi::TacviewCoreConfig cfg;  // reference 0/0, aircraft 10 Hz, weapons 20 Hz
    TerrainInfo terrain;
    g_theater.clear();
    if (terrain_capture(terrain)) {
        cfg.theater = terrain.id;
        g_theater = terrain.id;
        if (!terrain.full_name.empty()) cfg.title += " - " + terrain.full_name;
    }
    // The theatre's ini point is the centre of its square; without one the square sits over open ocean. A capture
    // failure only means no Theater=. The export gets the same placement.
    const acmi::Placement place =
        acmi::place_theatre(terrain.has_geo, terrain.geo_lon, terrain.geo_lat, (terrain.w - 1) * terrain.spacing,
                            (terrain.h - 1) * terrain.spacing);
    if (terrain.has_geo)
        LOG("tacview: theatre placed at its ini point (%.5f, %.5f); south-west corner (%.5f, %.5f)", terrain.geo_lon,
            terrain.geo_lat, place.ref_lon, place.ref_lat);
    else
        LOG("tacview: theatre has no usable ini position; using the open-ocean frame (%.1f, %.1f)", place.ref_lon,
            place.ref_lat);
    cfg.ref_lon = place.ref_lon;
    cfg.ref_lat = place.ref_lat;
    cfg.scale_lat = place.scale_lat;
    if (!g_writer->open(g_dir, local, iso, cfg, writer_log, display_path)) {
        LOG("tacview: not recording: could not start the writer");
        return StartResult::Failed;
    }
    terrain_export_start(terrain, place);
    g_aero_pending = !g_theater.empty();
    g_struct_pending = !g_theater.empty();
    g_struct_list.reset();
    g_struct_picked = false;
    g_struct_mark = g_struct_written = 0;
    g_watch_n = 0;
    g_last_poll = -1e300;
    g_ground_walked = false;
    g_ground_n = 0;
    g_player_known = false;
    g_struct_wait_from = std::nan("");
    g_active_n = 0;
    g_active_src = g_struct_pending ? mission_active_areas(g_game_areas, kMaxMissionAreas, &g_active_n) : MissionAreas::NotSingle;
    for (size_t i = 0; i < g_active_n; ++i) {
        g_active_xy[i][0] = g_game_areas[i].x;
        g_active_xy[i][1] = g_game_areas[i].y;
    }
    g_table_empty.store(false, std::memory_order_relaxed);
    g_recording = true;
    g_epoch = epoch;
    g_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_last_snapshot = -1e300;
    g_weapons_live = false;
    weapon_track_new_flight();
    g_dead_n = 0;
    g_wdead_n = 0;
    if (!g_walk_buf) g_walk_buf.reset(new (std::nothrow) acmi::TacviewRec[kWorldWalkMaxRecords]);
    g_walk_on = g_walk_buf && world_walk_start();
    if (!g_walk_buf) LOG("tacview: out of memory for the world walker; only aircraft and weapons are recorded");
    return StartResult::Started;
}

// The theatre's airfields, as one frame of their own, once the export thread has read them. Game thread.
void write_aerodromes(double t) {
    std::shared_ptr<const std::vector<AerodromeMark>> shared;
    switch (terrain_aerodromes(g_theater, shared)) {
        case StaticList::Pending: return;
        case StaticList::None: g_aero_pending = false; return;
        case StaticList::Ready: break;
    }
    const std::vector<AerodromeMark>& list = *shared;
    const size_t n = std::min(list.size(), kMaxAerodromes);
    if (!g_writer->begin_frame(n)) return;  // the ring was full (counted): tried again at the next step
    for (size_t i = 0; i < n; ++i) {
        acmi::TacviewRec& r = g_writer->slot(i);
        r.kind = acmi::TacviewRec::kObject;
        r.obj_class = acmi::TacviewRec::kAerodrome;
        r.side = 0;
        r.is_player = 0;
        r.parent_class = acmi::TacviewRec::kAircraft;
        r.destroyed = 0;
        r.seq = i + 1;
        r.parent_seq = 0;
        r.t = t;
        r.pos[0] = list[i].x;
        r.pos[1] = list[i].y;
        r.pos[2] = list[i].z;
        r.ang[0] = 0;
        r.ang[1] = static_cast<float>(list[i].heading);
        r.ang[2] = 0;
        const size_t k = acmi::utf8_prefix(list[i].name, acmi::kRecNameChars);
        std::memcpy(r.name, list[i].name.data(), k);
        r.name[k] = '\0';
        r.type[0] = '\0';
    }
    g_writer->commit_frame(n, t, acmi::TacviewRec::kSeriesAerodrome);
    g_aero_pending = false;
    LOG("tacview: %zu aerodromes recorded", n);
}

// The world walker's objects, as a frame of their own at the same game time. Game thread.
void write_world_frame(double t) {
    if (!g_walk_on) return;
    uint8_t mask = 0;
    const size_t n = world_walk_run(t, g_walk_buf.get(), kWorldWalkMaxRecords, &mask);
    if (!mask) return;
    if (!g_writer->begin_frame(n)) {  // the writer counts the dropped frame
        world_walk_frame_dropped(g_walk_buf.get(), n);
        return;
    }
    for (size_t i = 0; i < n; ++i) g_writer->slot(i) = g_walk_buf[i];
    g_writer->commit_frame(n, t, mask);
    if ((mask & acmi::TacviewRec::kSeriesGroundStatic) && !g_ground_walked) {
        g_ground_walked = true;
        for (size_t i = 0; i < n && g_ground_n < kWorldWalkMaxRecords; ++i) {
            const acmi::TacviewRec& r = g_walk_buf[i];
            if (r.obj_class != acmi::TacviewRec::kGroundStatic || r.destroyed || !std::isfinite(r.pos[0]) ||
                !std::isfinite(r.pos[1]))
                continue;
            g_ground_xy[g_ground_n][0] = r.pos[0];
            g_ground_xy[g_ground_n][1] = r.pos[1];
            ++g_ground_n;
        }
    }
}

// Decides, once per recording, which marks to write, and logs how. In a single mission each mark of a populated area is
// matched to the game's placed object by position: written only when the game drew it as a plain model (not when it
// made a live object, or skipped it for its year), and then watched for destruction. Game thread.
void pick_structures(const StructureList& list) {
    const size_t n = std::min(list.areas.size(), kMaxMissionAreas);
    acmi::AreaPick pick;
    pick.use_active = g_active_src == MissionAreas::Read;
    pick.active = g_active_xy;
    pick.n_active = g_active_n;
    pick.have_player = g_player_known;
    pick.player[0] = g_player_xy[0];
    pick.player[1] = g_player_xy[1];
    pick.range = kFallbackRangeMetres;
    pick.ground = g_ground_xy;
    pick.n_ground = g_ground_n;
    const size_t picked = acmi::pick_areas(list.areas.data(), n, pick, g_area_on);
    std::memset(g_mark_write, 0, sizeof(g_mark_write));
    size_t drawn = 0, live = 0, not_drawn = 0, unmatched = 0;
    for (size_t a = 0; a < n; ++a) {
        if (!g_area_on[a]) continue;
        const size_t first = list.first[a], end = std::min<size_t>(list.first[a + 1], kMaxStructures);
        const GameArea* g = nullptr;
        if (pick.use_active)
            for (size_t k = 0; k < g_active_n && !g; ++k)
                if (std::fabs(g_game_areas[k].x - list.areas[a].x) <= acmi::kAreaMatchMetres &&
                    std::fabs(g_game_areas[k].y - list.areas[a].y) <= acmi::kAreaMatchMetres)
                    g = &g_game_areas[k];
        size_t np = 0;
        if (g)
            for (int i = 0; i < g->n_placed && np < kMaxPlacedPerArea; ++i)
                if (mission_placed(*g, i, &g_placed[np])) {
                    g_placed_index[np] = i;
                    g_placed_xy[np][0] = g_placed[np].x;
                    g_placed_xy[np][1] = g_placed[np].y;
                    ++np;
                }
        std::memset(g_placed_taken, 0, np);
        for (size_t m = first; m < end; ++m) {
            if (!g) {
                g_mark_write[m] = 1;
                continue;
            }
            const size_t i =
                acmi::find_near(g_placed_xy, np, list.marks[m].x, list.marks[m].y, kPlacedMatchMetres, g_placed_taken);
            if (i >= np) {
                g_mark_write[m] = 1;  // not found in the game's list: written, but not watched
                ++unmatched;
                continue;
            }
            g_placed_taken[i] = 1;
            if (g_placed[i].object) {
                ++live;
            } else if (!g_placed[i].model) {
                ++not_drawn;
            } else {
                g_mark_write[m] = 1;
                ++drawn;
                if (g_watch_n < kMaxStructures) {
                    g_watch_rec[g_watch_n] = g_placed[i].record;
                    g_watch_seq[g_watch_n] = static_cast<uint32_t>(m + 1);
                    g_watch_dead[g_watch_n] = 0;
                    g_watch_xy[g_watch_n][0] = g_placed[i].x;
                    g_watch_xy[g_watch_n][1] = g_placed[i].y;
                    g_watch_area[g_watch_n] = static_cast<uint16_t>(g - g_game_areas);
                    g_watch_index[g_watch_n] = static_cast<uint16_t>(g_placed_index[i]);
                    ++g_watch_n;
                }
            }
        }
    }
    g_struct_picked = true;
    if (pick.use_active)
        LOG("tacview: structures from the %zu target areas the single mission populated (%zu matched): %zu drawn by the "
            "game, %zu left out as live objects, %zu not drawn this flight, %zu not found in the mission",
            g_active_n, picked, drawn, live, not_drawn, unmatched);
    else
        LOG("tacview: structures from %zu target areas near the player's start or holding live ground objects (%s); "
            "their destruction is not tracked",
            picked, g_active_src == MissionAreas::NotSingle ? "not a single mission" : "the mission's own set is unavailable");
}

// The theatre's placed structures, one chunk per step, after the first ground walk. The chunks name no series, so
// later frames never remove them; only the final frame (every series) does. Game thread.
void write_structures(double t) {
    if (std::isnan(g_struct_wait_from)) g_struct_wait_from = t;
    if (g_walk_on && !g_ground_walked && t - g_struct_wait_from < kStructureWaitSeconds) return;
    if (!g_struct_list) {
        switch (terrain_structures(g_theater, g_struct_list)) {
            case StaticList::Pending: return;
            case StaticList::None: g_struct_pending = false; return;
            case StaticList::Ready: break;
        }
    }
    const StructureList& list = *g_struct_list;
    if (!g_struct_picked) pick_structures(list);
    const size_t marks = std::min(list.marks.size(), kMaxStructures);
    size_t n = 0, mark = g_struct_mark;
    for (; mark < marks && n < kStructuresPerFrame; ++mark) n += g_mark_write[mark];
    if (n > 0) {
        if (!g_writer->can_take(n + kStructureHeadroom) || !g_writer->begin_frame(n)) return;  // not urgent: next step
        size_t used = 0;
        for (size_t m = g_struct_mark; used < n; ++m) {
            if (!g_mark_write[m]) continue;
            const StructureMark& sm = list.marks[m];
            acmi::TacviewRec& r = g_writer->slot(used++);
            r.kind = acmi::TacviewRec::kObject;
            r.obj_class = sm.bridge ? acmi::TacviewRec::kBridge : acmi::TacviewRec::kStructure;
            r.side = 0;
            r.is_player = 0;
            r.parent_class = acmi::TacviewRec::kAircraft;
            r.destroyed = 0;
            r.seq = m + 1;  // the mark's index: unique and stable within the theatre
            r.parent_seq = 0;
            r.t = t;
            r.pos[0] = sm.x;
            r.pos[1] = sm.y;
            r.pos[2] = sm.z;
            r.ang[0] = 0;
            r.ang[1] = static_cast<float>(sm.heading);
            r.ang[2] = 0;
            const size_t k = acmi::utf8_prefix(sm.name, acmi::kRecNameChars);
            std::memcpy(r.name, sm.name.data(), k);
            r.name[k] = '\0';
            r.type[0] = '\0';
        }
        g_writer->commit_frame(n, t, 0);
        g_struct_written += n;
    }
    g_struct_mark = mark;
    if (mark < marks) return;
    LOG("tacview: %zu structures queued%s", g_struct_written, list.marks.size() > kMaxStructures ? " (the cap)" : "");
    g_struct_pending = false;
    g_struct_list.reset();
}

// The drawn structures the game has destroyed since the last poll: a Destroyed marker each, in a frame of their own.
// Game thread; only after every chunk is out (a marker for a structure not yet written would be ignored).
void poll_destroyed_structures(double t) {
    if (g_struct_pending || g_watch_n == 0) return;
    if (t >= g_last_poll && t - g_last_poll < kDestroyedPollSeconds - 1e-9) return;
    g_last_poll = t;
    if (!mission_unchanged()) {  // the records may be gone: never read them again this recording
        g_watch_n = 0;
        LOG("tacview: the mission changed under the recording; structure destruction is no longer tracked");
        return;
    }
    if (!g_writer->can_take(kMaxDestroyedPerFrame)) return;  // ring busy: the next poll tries again
    const size_t n = mission_poll_destroyed(g_watch_rec, g_watch_n, g_watch_dead, g_watch_hits, kMaxDestroyedPerFrame);
    if (n == 0) return;
    if (!g_writer->begin_frame(n)) {  // not expected after can_take; tried again at the next poll
        for (size_t i = 0; i < n; ++i) g_watch_dead[g_watch_hits[i]] = 0;
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        acmi::TacviewRec& r = g_writer->slot(i);
        r.kind = acmi::TacviewRec::kObject;
        r.obj_class = acmi::TacviewRec::kStructure;
        r.destroyed = acmi::TacviewRec::kDestroyed;
        r.seq = g_watch_seq[g_watch_hits[i]];
        r.t = t;
    }
    g_writer->commit_frame(n, t, 0);
}

// Developer probe (Ctrl+Shift+X): sets the damage of the nearest watched, still standing structure to destroyed, so
// that poll_destroyed_structures() finds it as it would a real kill. Game thread, player step.
void destroy_nearest(const void* player, double t) {
    if (g_struct_pending) {
        LOG("tacview destroy test: ignored, the structures are still being written");
        return;
    }
    if (g_watch_n == 0) {
        LOG("tacview destroy test: ignored, no structure is watched (not a single mission, none drawn, or tracking "
            "stopped)");
        return;
    }
    if (!mission_unchanged()) {
        LOG("tacview destroy test: ignored, the mission changed under the recording");
        return;
    }
    double pos[3];
    float ang[3];
    if (!read_pose(player, kOffPos, kOffAngles, pos, ang) || !std::isfinite(pos[0]) || !std::isfinite(pos[1])) {
        LOG("tacview destroy test: ignored, the player's position is unreadable");
        return;
    }
    for (size_t i = 0; i < g_watch_n; ++i) {
        const int d = g_watch_dead[i] ? -1 : mission_placed_damage(g_watch_rec[i]);
        g_destroy_skip[i] = d < 0 || d >= 100;
    }
    const size_t i = acmi::find_near(g_watch_xy, g_watch_n, pos[0], pos[1], HUGE_VAL, g_destroy_skip);
    if (i >= g_watch_n) {
        LOG("tacview destroy test: ignored, every one of the %zu watched structures is destroyed or unreadable", g_watch_n);
        return;
    }
    int before = -1;
    if (!mission_set_destroyed(g_watch_rec[i], &before)) {
        LOG("tacview destroy test: could not write the damage of placed record %p (damage %d)", g_watch_rec[i], before);
        return;
    }
    const uint32_t seq = g_watch_seq[i];
    std::string name = "?";
    std::shared_ptr<const StructureList> list;
    if (terrain_structures(g_theater, list) == StaticList::Ready && list && seq - 1 < list->marks.size())
        name = list->marks[seq - 1].name;
    const GameArea& a = g_game_areas[g_watch_area[i]];
    const double dx = g_watch_xy[i][0] - pos[0], dy = g_watch_xy[i][1] - pos[1];
    LOG("tacview destroy test: damage %d -> 100 at placed record %p (area %u at (%.0f, %.0f), placed %u of %d): "
        "structure %u \"%s\" at (%.0f, %.0f), %.0f m from the player; Tacview id %x. The destruction poll (every %.0f s "
        "of game time, t=%.1f) should write its Destroyed event and explosion",
        before, g_watch_rec[i], g_watch_area[i], a.x, a.y, g_watch_index[i], a.n_placed, seq, name.c_str(),
        g_watch_xy[i][0], g_watch_xy[i][1], std::sqrt(dx * dx + dy * dy), acmi::kStructureIdBase + seq,
        kDestroyedPollSeconds, t);
}

// A start waits (retries each player step) while the previous writer drains, so the game thread never blocks.
void on_step(void* aircraft, float /*dt*/, bool is_player) {
    if (g_recording) {
        if (sim_player_epoch() != g_epoch) {
            stop_recording("player lost", true);
        } else if (g_table_empty.exchange(false, std::memory_order_acq_rel)) {
            stop_recording("table empty", true);
        } else if (!g_writer->accepting()) {
            g_skip_valid = true;  // the writer already logged why (write failure, size cap, no file)
            g_skip_epoch = g_epoch;
            stop_recording("writer stopped");
        }
    }
    if (!is_player) return;
    const bool destroy = g_destroy_request.exchange(false, std::memory_order_relaxed);
    if (destroy && !g_recording) LOG("tacview destroy test: ignored, no Tacview recording is running");
    if (!g_recording) {
        if (config::tacview_recording_peek() != 1) return;
        const uint32_t ep = sim_player_epoch();
        if (g_skip_valid && g_skip_epoch == ep) return;
        const StartResult sr = start_recording(ep);
        if (sr == StartResult::Retry) return;  // not marked: tried again at the next player step
        if (sr == StartResult::Failed) {
            g_skip_valid = true;
            g_skip_epoch = ep;
            return;
        }
    }
    const double t = sim_game_time();
    if (g_aero_pending) write_aerodromes(t);
    const double interval = g_weapons_live ? kSnapshotIntervalWeapons : kSnapshotInterval;
    if (!(t >= g_last_snapshot && t - g_last_snapshot < interval - 1e-9)) {
        g_last_snapshot = t;
        SnapCtx c;
        c.t = t;
        probe_with_aircraft(snapshot_cb, &c);
    }
    write_world_frame(t);
    if (g_struct_pending && !g_player_known) {
        double pos[3];
        float ang[3];
        if (read_pose(aircraft, kOffPos, kOffAngles, pos, ang) && std::isfinite(pos[0]) && std::isfinite(pos[1])) {
            g_player_known = true;
            g_player_xy[0] = pos[0];
            g_player_xy[1] = pos[1];
        }
    }
    if (g_struct_pending) write_structures(t);
    poll_destroyed_structures(t);
    if (destroy) destroy_nearest(aircraft, t);
}

}  // namespace

void tacview_recorder_on_modules_loaded() {
    if (g_registered.load(std::memory_order_relaxed) || g_registered.exchange(true)) return;
    if (!sim_register_step_callback(on_step)) LOG("tacview: no free step callback slot; the recorder is off");
}

void tacview_recorder_table_empty() {
    // On the recording's thread, close now: no aircraft update follows a flight's end, and the next menu moment may
    // be the next flight. Elsewhere, leave it to the next step or menu moment.
    if (GetCurrentThreadId() == g_thread.load(std::memory_order_relaxed) && g_recording) {  // thread first
        g_table_empty.store(false, std::memory_order_relaxed);
        stop_recording("table empty", true);
        return;
    }
    g_table_empty.store(true, std::memory_order_release);
}

namespace {
void count_cb(const LiveAircraft*, size_t n, void* ctx) { *static_cast<size_t*>(ctx) = n; }
}  // namespace

void tacview_recorder_menu_moment() {
    if (!g_recording) return;
    if (config::tacview_recording_peek() != 1) {  // the setting was turned Off: never leave a file open
        stop_recording("setting off", true);
        return;
    }
    if (sim_player_epoch() != g_epoch) {
        stop_recording("player lost", true);
        return;
    }
    size_t live = 1;
    probe_with_aircraft(count_cb, &live);
    if (live == 0 || g_table_empty.exchange(false, std::memory_order_acq_rel)) {
        stop_recording("table empty", true);
        return;
    }
    if (!g_writer->accepting()) stop_recording("writer stopped");
}

void tacview_destroy_nearest_request() { g_destroy_request.store(true, std::memory_order_relaxed); }

void tacview_recorder_shutdown(bool process_exit) {
    terrain_export_shutdown();
    // At process exit the compressor thread is already gone, possibly holding its lock: leave the plain file.
    if (!process_exit) acmi::zip_queue_shutdown();
    if (!g_writer) return;
    g_recording = false;
    g_writer->close(true, "process exit");
}

}  // namespace LazyWorks::SF2Patch::hooks
