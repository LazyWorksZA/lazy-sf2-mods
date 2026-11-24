// The Tacview recorder's formatting core: turns frames of aircraft states into ACMI 2.2 text. Pure (no Windows, no
// game): the game side fills TacviewRec slots, the writer thread (tacview_writer.h) feeds them here, and the tests
// feed synthetic ones.
//
// A frame is the state of every live object at one game time: add() for each object, then end_frame(). The core
//   - writes an object's first transform with its properties (Name, Pilot, Type, Coalition, Color),
//   - limits each object to max_rate transforms per second,
//   - writes "-id" for an object that was in an earlier frame and is not in this one,
//   - keeps frame times monotonic (relative to the first frame) and never writes an empty frame.
// The player (rec.is_player) has id 1 and Pilot=Player; any other aircraft has id 0x10000 + seq; a weapon has id
// 0x1000000 + seq (its own sequence numbers, so the two series can never collide).
// A weapon (obj_class other than kAircraft) is written as Weapon+Missile, Weapon+Bomb or Weapon+Rocket, with
// Parent=<id> when parent_seq names an aircraft, and the parent's coalition and colour when the record has no side.
// Each class has its own rate limit: aircraft 10 Hz, weapons 20 Hz (weapons move fast).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hooks/aircraft_side.h"

namespace lazysf2::acmi {

constexpr uint32_t kPlayerId = 1;
constexpr uint32_t kOtherIdBase = 0x10000;
constexpr uint32_t kWeaponIdBase = 0x1000000;
constexpr size_t kRecNameChars = 63;
constexpr size_t kRecTypeChars = 31;  // aircraft type name (Tacview model key), e.g. "F-100D"

// One object's state in a frame, as copied by the game thread: plain data.
struct TacviewRec {
    static constexpr uint8_t kObject = 0, kEndFrame = 1;
    static constexpr uint8_t kAircraft = 0, kMissile = 1, kBomb = 2, kRocket = 3;
    uint8_t kind = kObject;  // kObject, or kEndFrame (then only t is used)
    uint8_t side = 0;        // hooks::AircraftSide
    uint8_t is_player = 0;
    uint8_t obj_class = kAircraft;  // kAircraft, kMissile, kBomb or kRocket
    uint64_t seq = 0;         // the tracker's sequence number: unique and never reused within its class
    uint64_t parent_seq = 0;  // weapons: the shooting aircraft's seq, or 0 for none
    double t = 0;      // game time, seconds
    double pos[3] = {};
    float ang[3] = {};  // pitch, heading, roll in radians
    char name[kRecNameChars + 1] = {};
    char type[kRecTypeChars + 1] = {};  // aircraft only: the type name, empty when unknown (always empty for weapons)
};
static_assert(sizeof(TacviewRec) == 168, "TacviewRec is copied through the ring: keep it 168 bytes");

struct TacviewCoreConfig {
    double ref_lat = 0;    // degrees; origin of the local flat mapping
    double ref_lon = 0;
    double max_rate = 10;         // aircraft: transforms per second per object at most; 0 or less means no limit
    double max_rate_weapon = 20;  // weapons: the same, for every object that is not an aircraft
};

struct TacviewCoreStats {
    uint64_t transforms = 0;
    uint64_t removals = 0;
    uint64_t objects = 0;       // distinct objects described
    uint64_t rate_limited = 0;  // transforms skipped by the rate limit
    uint64_t bad_samples = 0;   // objects skipped for a non-finite value
};

class TacviewCore {
public:
    explicit TacviewCore(const TacviewCoreConfig& cfg);

    // "FileType...Title" text. reference_time is the real UTC start, e.g. 2026-11-24T20:15:00Z.
    void header(std::string& out, const char* reference_time) const;
    // One object of the current frame. Only kObject records.
    void add(const TacviewRec& r);
    // Ends the frame at game time t and appends its text to out.
    void end_frame(double t, std::string& out);

    // For a fresh core that continues a file whose first core was lost (process exit): same time origin, same last
    // frame, so times stay monotonic. Objects are described again, which ACMI allows.
    void seed_time(double t0, long long last_ms);
    bool has_origin() const { return have_t0_; }
    double origin() const { return t0_; }
    long long last_ms() const { return last_ms_; }
    const TacviewCoreStats& stats() const { return stats_; }
    size_t live_objects() const { return objs_.size(); }

private:
    struct State {
        uint64_t seq;
        uint8_t obj_class;
        uint8_t side;
        uint32_t id;
        double last_t;
        uint64_t seen;  // number of the last frame it was in
    };
    TacviewCoreConfig cfg_;
    double lon_scale_ = 0, lat_scale_ = 0;
    double min_dt_ = 0, min_dt_weapon_ = 0;
    std::vector<State> objs_;
    std::string pending_;  // transform lines of the current frame
    uint64_t frame_no_ = 1;
    bool have_t0_ = false;
    double t0_ = 0;
    double last_t_ = 0;  // monotonic clamp of frame times
    long long last_ms_ = -1;
    TacviewCoreStats stats_;
};

}  // namespace lazysf2::acmi
