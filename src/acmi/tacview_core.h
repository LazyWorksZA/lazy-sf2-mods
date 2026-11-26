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
// A record with destroyed set is a marker: if that object is known, the core writes "0,Event=Destroyed|id|" before its
// removal (kDestroyed only; it is not counted as seen, so the removal follows in the same frame), and an explosion
// object (Misc+Explosion with Radius) at its last written position, removed kExplosionLifeSeconds later or by a frame
// that lists every series (the last one). An unknown object is ignored.
// A weapon (obj_class other than kAircraft) is written as Weapon+Missile, Weapon+Bomb or Weapon+Rocket, with
// Parent=<id> when parent_seq names an aircraft, and the parent's coalition and colour when the record has no side.
// A ground object, vehicle or ship whose pose has not changed since its last written transform gets no new line
// (Tacview keeps the last state); it is still sampled, which drives its removal.
// A bullet (0x4000000+seq) is Projectile+Bullet with Parent=<id> when parent_seq names a known shooter.
// Aircraft and weapons have a rate limit each (10 Hz and 20 Hz: weapons move fast). Ground objects (0x2000000+seq) and
// countermeasures (0x3000000+seq) have none here: the world walker samples them at its own rates. Ground objects are
// Ground+Static+Building, Ground+Vehicle or Sea+Watercraft with the type name as Name; flares and chaff are Misc+Decoy+...
// An aerodrome (0x6000000+seq) is a static Ground+Static+Aerodrome with its name and heading, written once: the recorder
// lists it in one frame of its own, and only the final frame (every series) removes it.
// A structure (0x7000000+seq) is a placed building or bridge from the theatre's inis: Ground+Static+Building or
// Ground+Static+Bridge, written once and never updated; the recorder lists them over several frames that name no series,
// so only the final frame removes them. A structure within kStructureDedupeMetres of a live ground object (a
// GroundObject the core already has a position for) is skipped: the game made that one a real object. A destroyed
// marker for a written structure writes its Destroyed event and an explosion, and removes it.
// A transform line writes only the T components whose text changed since the last line of that object (ACMI 2.2 lets a
// component stay empty); every '|' is kept, and an object's first line is complete.
// An aircraft or weapon record with lock_mode set names its locked aircraft (lock_seq): the core writes
// "id,LockedTarget=<target id>,LockedTargetMode=1" when the target changes, and "id,LockedTargetMode=0,LockedTarget="
// when the lock is lost; a target the core has not described yet counts as no lock. Nothing is written while it stays.
// A frame says which series it lists completely (end_frame's series_mask), so a 1 Hz series is not removed in between.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "acmi/acmi_format.h"
#include "hooks/aircraft_side.h"

namespace LazyWorks::SF2Patch::acmi {

constexpr uint32_t kPlayerId = 1;
constexpr uint32_t kOtherIdBase = 0x10000;
constexpr uint32_t kWeaponIdBase = 0x1000000;
constexpr uint32_t kGroundIdBase = 0x2000000;  // ground objects, vehicles and ships
constexpr uint32_t kCounterMeasureIdBase = 0x3000000;  // flares and chaff
constexpr uint32_t kBulletIdBase = 0x4000000;  // tracer rounds
constexpr uint32_t kExplosionIdBase = 0x5000000;  // explosion objects, numbered by the core
constexpr uint32_t kAerodromeIdBase = 0x6000000;  // airfields, written once per recording
constexpr uint32_t kStructureIdBase = 0x7000000;  // placed buildings and bridges, written once per recording
constexpr double kStructureDedupeMetres = 15.0;    // horizontal distance
// Explosion size (the Radius property, metres) by what blew up, and how long the object stays (game seconds).
constexpr double kExplosionRadiusBomb = 30, kExplosionRadiusRocket = 8, kExplosionRadiusMissile = 12;
constexpr double kExplosionRadiusAircraft = 20, kExplosionRadiusGround = 25;
constexpr double kExplosionLifeSeconds = 2.0;
constexpr size_t kRecNameChars = 63;
constexpr size_t kRecTypeChars = 31;  // aircraft type name (Tacview model key), e.g. "F-100D"

/// <summary>One object's state in a frame, as copied by the game thread: plain data.</summary>
struct TacviewRec {
    static constexpr uint8_t kObject = 0, kEndFrame = 1;
    static constexpr uint8_t kAircraft = 0, kMissile = 1, kBomb = 2, kRocket = 3;
    static constexpr uint8_t kGroundStatic = 4, kGroundVehicle = 5, kShip = 6, kFlare = 7, kChaff = 8, kBullet = 9,
                             kAerodrome = 10, kStructure = 11, kBridge = 12;
    /// <summary>The kinds of object that share an id range and a sequence counter.</summary>
    enum class Family : uint8_t { Aircraft, Weapon, Ground, CounterMeasure, Bullet, Aerodrome, Structure };
    static constexpr Family family_of(uint8_t cls) {
        if (cls == kAircraft) return Family::Aircraft;
        if (cls >= kGroundStatic && cls <= kShip) return Family::Ground;
        if (cls == kFlare || cls == kChaff) return Family::CounterMeasure;
        if (cls == kBullet) return Family::Bullet;
        if (cls == kAerodrome) return Family::Aerodrome;
        if (cls == kStructure || cls == kBridge) return Family::Structure;
        return Family::Weapon;  // missile, bomb, rocket, and any unknown value
    }
    /// <summary>Frame series: a frame lists some series completely, and only those can lose objects (removal).</summary>
    static constexpr uint8_t kSeriesAircraft = 1, kSeriesWeapon = 2, kSeriesGroundStatic = 4, kSeriesGroundMobile = 8,
                             kSeriesCounterMeasure = 16, kSeriesBullet = 32, kSeriesAerodrome = 64, kSeriesStructure = 128,
                             kSeriesAll = 0xff;
    static constexpr uint8_t series_of(uint8_t cls) {
        switch (family_of(cls)) {
            case Family::Aircraft: return kSeriesAircraft;
            case Family::Ground: return cls == kGroundStatic ? kSeriesGroundStatic : kSeriesGroundMobile;
            case Family::CounterMeasure: return kSeriesCounterMeasure;
            case Family::Bullet: return kSeriesBullet;
            case Family::Aerodrome: return kSeriesAerodrome;
            case Family::Structure: return kSeriesStructure;
            default: return kSeriesWeapon;
        }
    }
    uint8_t kind = kObject;  // kObject, or kEndFrame (then only t is used)
    uint8_t side = 0;        // hooks::AircraftSide
    uint8_t is_player = 0;
    uint8_t obj_class = kAircraft;  // kAircraft, kMissile, kBomb, kRocket, kGroundStatic ... kChaff
    uint8_t series_mask = kSeriesAll;  // kEndFrame only: the series this frame lists completely
    uint8_t parent_class = kAircraft;  // the class of the parent: a weapon's is an aircraft, a bullet's may be a ground object
    uint8_t destroyed = 0;  // a marker, not a sample, for the object (seq, class); only those are used: kDestroyed or kDetonated
    static constexpr uint8_t kDestroyed = 1, kDetonated = 2;
    uint8_t lock_mode = 0;  // aircraft and weapons: 1 when lock_seq names the locked target, 0 for none
    uint64_t seq = 0;         // the tracker's sequence number: unique and never reused within its class
    uint64_t parent_seq = 0;  // weapons and bullets: the shooter's seq (in the family of parent_class), or 0 for none
    double t = 0;      // game time, seconds
    double pos[3] = {};
    float ang[3] = {};  // pitch, heading, roll in radians
    char name[kRecNameChars + 1] = {};
    char type[kRecTypeChars + 1] = {};  // aircraft only: the type name, empty when unknown (always empty for weapons)
    uint32_t lock_seq = 0;  // with lock_mode: the locked aircraft's seq (aircraft family)
};
static_assert(sizeof(TacviewRec) == 168, "TacviewRec is copied through the ring: keep it 168 bytes");

struct TacviewCoreConfig {
    double ref_lat = 0;    // degrees; origin of the local flat mapping
    double ref_lon = 0;
    double scale_lat = std::numeric_limits<double>::quiet_NaN();  // latitude of the longitude scale; NaN means ref_lat
    double max_rate = 10;         // aircraft: transforms per second per object at most; 0 or less means no limit
    double max_rate_weapon = 20;  // weapons: the same, for every object that is not an aircraft
    std::string title = "Strike Fighters 2";
    std::string theater;  // the Tacview custom terrain id; empty writes no Theater=
};

struct TacviewCoreStats {
    uint64_t transforms = 0;
    uint64_t removals = 0;
    uint64_t destroyed_events = 0;
    uint64_t explosions = 0;
    uint64_t unchanged = 0;     // transforms skipped because no formatted component had changed
    uint64_t objects = 0;       // distinct objects described
    uint64_t rate_limited = 0;  // transforms skipped by the rate limit
    uint64_t bad_samples = 0;   // objects skipped for a non-finite value
    uint64_t structures = 0;          // structures written
    uint64_t structures_skipped = 0;  // structures not written: a live ground object stands there
    uint64_t structures_destroyed = 0;
    uint64_t lock_changes = 0;  // LockedTarget lines written (locks and clears)
};

class TacviewCore {
public:
    explicit TacviewCore(const TacviewCoreConfig& cfg);

    /// <summary>"FileType...Title" text. reference_time is the real UTC start, e.g. 2026-11-24T20:15:00Z.</summary>
    void header(std::string& out, const char* reference_time) const;
    /// <summary>One object of the current frame. Only kObject records.</summary>
    void add(const TacviewRec& r);
    /// <summary>
    /// Ends the frame at game time t and appends its text to out. Only objects of the series in series_mask can be
    /// removed: a frame that lists just some series leaves the others alone.
    /// </summary>
    void end_frame(double t, std::string& out, uint8_t series_mask = TacviewRec::kSeriesAll);

    /// <summary>
    /// For a fresh core that continues a file whose first core was lost (process exit): same time origin, same last
    /// frame, so times stay monotonic. Objects are described again, which ACMI allows.
    /// </summary>
    void seed_time(double t0, long long last_ms);
    /// <summary>For the same fresh core: continue the explosion numbering, so no id of the lost core is reused.</summary>
    void seed_explosions(uint32_t next) { if (next > next_explosion_) next_explosion_ = next; }
    uint32_t next_explosion() const { return next_explosion_; }
    bool has_origin() const { return have_t0_; }
    double origin() const { return t0_; }
    long long last_ms() const { return last_ms_; }
    const TacviewCoreStats& stats() const { return stats_; }
    size_t live_objects() const { return objs_.size() + statics_.size(); }

private:
    struct State {
        uint64_t seq;
        uint8_t obj_class;
        uint8_t side;
        uint8_t series;
        bool destroyed;  // its Destroyed event is already written
        uint32_t id;
        double last_t;
        uint64_t seen;  // number of the last frame it was in
        double last_pos[3];  // the pose last sampled (where an explosion goes)
        float last_ang[3];
        bool has_last;
        uint32_t lock_id;  // the LockedTarget last written; 0 for none
        uint32_t want_lock;    // this frame's lock_seq (0 for none), resolved at the end of the frame
        uint64_t lock_frame;   // the frame want_lock was sampled in
        TransformText text;  // the last written text of each T component (delta encoding)
    };
    TacviewCoreConfig cfg_;
    double lon_scale_ = 0, lat_scale_ = 0;
    double min_dt_ = 0, min_dt_weapon_ = 0;
    std::vector<State> objs_;
    struct Static {
        uint64_t seq;
        uint32_t id;
        uint64_t seen;
        double pos[3];  // where an explosion goes
    };
    std::vector<Static> statics_;  // structures: kept out of objs_, so their thousands never slow the per-object lookup
    std::vector<uint32_t> dead_statics_;  // destroyed in the current frame: removed at its end
    std::unordered_map<uint64_t, size_t> static_index_;  // seq to index in statics_
    void add_structure(const TacviewRec& r);
    std::string pending_;  // transform lines of the current frame
    std::string locks_;    // lock lines of the current frame: after the transforms, so every object is described first
    void resolve_locks();
    struct Explosion {
        uint32_t id;
        double remove_at;  // game time
        uint64_t frame;    // the frame that wrote it
    };
    std::vector<Explosion> explosions_;
    uint32_t next_explosion_ = 1;
    void add_explosion(const double pos[3], double radius, double t);
    std::string events_;   // Destroyed events of the current frame: written before the removals
    uint64_t frame_no_ = 1;
    bool have_t0_ = false;
    double t0_ = 0;
    double last_t_ = 0;  // monotonic clamp of frame times
    long long last_ms_ = -1;
    TacviewCoreStats stats_;
};

}  // namespace LazyWorks::SF2Patch::acmi
