// The Tacview recorder's formatting core: turns frames of aircraft states into ACMI 2.2 text. Pure (no Windows, no
// game): the game side fills TacviewRec slots, the writer thread (tacview_writer.h) feeds them here, and the tests
// feed synthetic ones.
//
// A frame is the state of every live object at one game time: add() for each object, then end_frame(). The core
//   - writes an object's first transform with its properties (Name, Pilot, Type, Coalition, Color),
//   - limits each object to max_rate transforms per second,
//   - writes "-id" for an object that was in an earlier frame and is not in this one,
//   - keeps frame times monotonic (relative to the first frame) and never writes an empty frame.
// The player (rec.is_player) has id 1 and Pilot=Player; any other object has id 0x10000 + seq.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hooks/aircraft_side.h"

namespace lazysf2::acmi {

constexpr uint32_t kPlayerId = 1;
constexpr uint32_t kOtherIdBase = 0x10000;
constexpr size_t kRecNameChars = 63;

// One object's state in a frame, as copied by the game thread: plain data.
struct TacviewRec {
    static constexpr uint8_t kObject = 0, kEndFrame = 1;
    uint8_t kind = kObject;  // kObject, or kEndFrame (then only t is used)
    uint8_t side = 0;        // hooks::AircraftSide
    uint8_t is_player = 0;
    uint8_t pad = 0;
    uint64_t seq = 0;  // the tracker's construction sequence number: unique and never reused
    double t = 0;      // game time, seconds
    double pos[3] = {};
    float ang[3] = {};  // pitch, heading, roll in radians
    char name[kRecNameChars + 1] = {};
};

struct TacviewCoreConfig {
    double ref_lat = 0;    // degrees; origin of the local flat mapping
    double ref_lon = 0;
    double max_rate = 10;  // transforms per second per object at most; 0 or less means no limit
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
        uint32_t id;
        double last_t;
        uint64_t seen;  // number of the last frame it was in
    };
    TacviewCoreConfig cfg_;
    double lon_scale_ = 0, lat_scale_ = 0;
    double min_dt_ = 0;
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
