#include "acmi/tacview_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "acmi/acmi_format.h"
#include "acmi/object_types.h"

namespace LazyWorks::SF2Patch::acmi {

namespace {
bool finite3(const double* p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
bool finite3(const float* p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
}  // namespace

TacviewCore::TacviewCore(const TacviewCoreConfig& cfg) : cfg_(cfg) {
    cfg_.ref_lat = std::clamp(cfg_.ref_lat, -89.0, 89.0);
    cfg_.ref_lon = std::clamp(cfg_.ref_lon, -180.0, 180.0);
    const Mapping m = make_mapping(std::isnan(cfg_.scale_lat) ? cfg_.ref_lat : std::clamp(cfg_.scale_lat, -89.0, 89.0));
    lon_scale_ = m.lon_scale;
    lat_scale_ = m.lat_scale;
    min_dt_ = cfg_.max_rate > 0 ? 1.0 / cfg_.max_rate : 0.0;
    min_dt_weapon_ = cfg_.max_rate_weapon > 0 ? 1.0 / cfg_.max_rate_weapon : 0.0;
    objs_.reserve(64);
}

void TacviewCore::header(std::string& out, const char* reference_time) const {
    append_header(out, reference_time, cfg_.ref_lon, cfg_.ref_lat, cfg_.title.c_str(), cfg_.theater.c_str());
}

void TacviewCore::seed_time(double t0, long long last_ms) {
    have_t0_ = true;
    t0_ = t0;
    last_ms_ = last_ms;
    last_t_ = t0 + static_cast<double>(last_ms) / 1000.0;
}

void TacviewCore::add(const TacviewRec& r) {
    if (r.kind != TacviewRec::kObject) return;
    // Each family has its own seq counter: the family is part of the identity.
    const auto family = TacviewRec::family_of(r.obj_class);
    if (family == TacviewRec::Family::Structure) {
        add_structure(r);
        return;
    }
    const bool weapon = family == TacviewRec::Family::Weapon;
    State* s = nullptr;
    for (State& o : objs_) {
        if (o.seq == r.seq && TacviewRec::family_of(o.obj_class) == family) {
            s = &o;
            break;
        }
    }
    if (r.destroyed) {
        if (s && !s->destroyed) {
            s->destroyed = true;
            if (r.destroyed == TacviewRec::kDestroyed) {
                append_event_destroyed(events_, s->id);
                ++stats_.destroyed_events;
            }
            if (s->has_last) {
                double radius = kExplosionRadiusMissile;
                switch (s->obj_class) {
                    case TacviewRec::kAircraft: radius = kExplosionRadiusAircraft; break;
                    case TacviewRec::kBomb: radius = kExplosionRadiusBomb; break;
                    case TacviewRec::kRocket: radius = kExplosionRadiusRocket; break;
                    default:
                        if (TacviewRec::family_of(s->obj_class) == TacviewRec::Family::Ground) radius = kExplosionRadiusGround;
                        break;
                }
                add_explosion(s->last_pos, radius, r.t);
            }
        }
        return;  // not seen in this frame: end_frame removes it
    }
    if (!std::isfinite(r.t) || !finite3(r.pos) || !finite3(r.ang)) {
        ++stats_.bad_samples;
        if (s) s->seen = frame_no_;  // still present: no remove and re-add for one bad sample
        return;
    }
    const bool is_new = s == nullptr;
    if (is_new) {
        const uint32_t n = static_cast<uint32_t>(r.seq);
        uint32_t id = 0;
        switch (family) {
            case TacviewRec::Family::Aircraft: id = r.is_player ? kPlayerId : kOtherIdBase + n; break;
            case TacviewRec::Family::Weapon: id = kWeaponIdBase + n; break;
            case TacviewRec::Family::Ground: id = kGroundIdBase + n; break;
            case TacviewRec::Family::CounterMeasure: id = kCounterMeasureIdBase + n; break;
            case TacviewRec::Family::Bullet: id = kBulletIdBase + n; break;
            case TacviewRec::Family::Aerodrome: id = kAerodromeIdBase + n; break;
        }
        objs_.push_back({r.seq, r.obj_class, r.side, TacviewRec::series_of(r.obj_class), false, id, -1e300, 0});
        s = &objs_.back();
    }
    s->seen = frame_no_;  // present in this frame, even if its transform is skipped below
    const double min_dt = family == TacviewRec::Family::Aircraft ? min_dt_
                          : weapon                              ? min_dt_weapon_
                                                                : 0.0;  // the walker's own due times
    if (!is_new && r.t - s->last_t < min_dt - 1e-9) {
        ++stats_.rate_limited;
        return;
    }
    s->last_t = r.t;
    if (family == TacviewRec::Family::Ground && !is_new && s->has_last &&
        std::memcmp(s->last_pos, r.pos, sizeof(r.pos)) == 0 && std::memcmp(s->last_ang, r.ang, sizeof(r.ang)) == 0) {
        ++stats_.unchanged;
        return;
    }
    std::memcpy(s->last_pos, r.pos, sizeof(r.pos));  // the last written pose: where an explosion goes
    std::memcpy(s->last_ang, r.ang, sizeof(r.ang));
    s->has_last = true;

    const Mapping m{lon_scale_, lat_scale_};
    TransformText cur;
    format_transform(cur, r.pos, r.ang, m);
    if (!append_transform_delta(pending_, s->id, cur, s->text, is_new)) {
        ++stats_.unchanged;  // nothing differs in text: Tacview keeps the last state
        return;
    }
    if (is_new) {
        ++stats_.objects;
        char name[kRecNameChars + 1];
        std::copy(r.name, r.name + kRecNameChars, name);
        name[kRecNameChars] = '\0';
        char type[kRecTypeChars + 1];  // outlives p: p.name may point at it
        std::copy(r.type, r.type + kRecTypeChars, type);
        type[kRecTypeChars] = '\0';
        ObjectProps p;
        p.name = name;
        char parent_id[16] = {};
        auto side = static_cast<hooks::AircraftSide>(r.side);
        if (weapon || family == TacviewRec::Family::Bullet) {
            if (weapon) {
                p.type = r.obj_class == TacviewRec::kBomb     ? "Weapon+Bomb"
                         : r.obj_class == TacviewRec::kRocket ? "Weapon+Rocket"
                                                               : "Weapon+Missile";
            } else {
                p.name = nullptr;
                p.type = "Projectile+Bullet";
            }
            if (r.parent_seq != 0) {
                const auto pfam = TacviewRec::family_of(r.parent_class);
                uint32_t pid = (pfam == TacviewRec::Family::Ground ? kGroundIdBase : kOtherIdBase) +
                               static_cast<uint32_t>(r.parent_seq);  // a shooter we have not seen
                for (const State& o : objs_) {
                    if (o.seq == r.parent_seq && TacviewRec::family_of(o.obj_class) == pfam) {
                        pid = o.id;
                        if (side == hooks::AircraftSide::Unknown) side = static_cast<hooks::AircraftSide>(o.side);
                        break;
                    }
                }
                std::snprintf(parent_id, sizeof(parent_id), "%x", pid);
                p.parent = parent_id;
            }
        } else if (family == TacviewRec::Family::Ground) {
            p.type = ground_type_tags(r.obj_class, name);
        } else if (family == TacviewRec::Family::Aerodrome) {
            side = hooks::AircraftSide::Unknown;  // an airfield has no coalition here
            p.type = "Ground+Static+Aerodrome";
        } else if (family == TacviewRec::Family::CounterMeasure) {
            p.name = nullptr;
            side = hooks::AircraftSide::Unknown;  // decoys have no coalition
            p.type = r.obj_class == TacviewRec::kFlare ? "Misc+Decoy+Flare" : "Misc+Decoy+Chaff";
        } else {
            if (type[0]) {  // type known: the model key is the Name, the callsign is the Pilot (the player too)
                p.name = type;
                p.pilot = name;
            } else {
                p.pilot = r.is_player ? "Player" : name;
            }
            p.type = "Air+FixedWing";
        }
        switch (side) {
            case hooks::AircraftSide::Friendly:
                p.coalition = "Allies";
                p.color = "Blue";
                break;
            case hooks::AircraftSide::Enemy:
                p.coalition = "Enemies";
                p.color = "Red";
                break;
            case hooks::AircraftSide::Neutral:
                p.coalition = "Neutrals";
                p.color = "Grey";
                break;
            default:
                break;  // unknown side: leave both unset
        }
        append_props(pending_, p);
    }
    pending_ += '\n';
    ++stats_.transforms;
}

void TacviewCore::add_structure(const TacviewRec& r) {
    const auto known = static_index_.find(r.seq);
    if (r.destroyed) {
        // Destroyed (a marker): the event, an explosion where it stood, and its removal in this frame. Unknown: ignored.
        if (known == static_index_.end()) return;
        const size_t i = known->second;
        const Static st = statics_[i];
        if (r.destroyed == TacviewRec::kDestroyed) {
            append_event_destroyed(events_, st.id);
            ++stats_.destroyed_events;
        }
        add_explosion(st.pos, kExplosionRadiusGround, r.t);
        dead_statics_.push_back(st.id);
        static_index_.erase(known);
        if (i + 1 != statics_.size()) {
            statics_[i] = statics_.back();
            static_index_[statics_[i].seq] = i;
        }
        statics_.pop_back();
        ++stats_.structures_destroyed;
        return;
    }
    if (known != static_index_.end()) {
        statics_[known->second].seen = frame_no_;  // already written: Tacview keeps it
        return;
    }
    if (!std::isfinite(r.t) || !finite3(r.pos) || !finite3(r.ang)) {
        ++stats_.bad_samples;
        return;
    }
    constexpr double kNear2 = kStructureDedupeMetres * kStructureDedupeMetres;
    for (const State& o : objs_) {
        if (o.obj_class != TacviewRec::kGroundStatic || !o.has_last) continue;
        const double dx = o.last_pos[0] - r.pos[0], dy = o.last_pos[1] - r.pos[1];
        if (dx * dx + dy * dy <= kNear2) {
            ++stats_.structures_skipped;
            return;
        }
    }
    const uint32_t id = kStructureIdBase + static_cast<uint32_t>(r.seq);
    static_index_.emplace(r.seq, statics_.size());
    statics_.push_back({r.seq, id, frame_no_, {r.pos[0], r.pos[1], r.pos[2]}});
    append_transform(pending_, id, r.pos, r.ang, Mapping{lon_scale_, lat_scale_});
    char name[kRecNameChars + 1];
    std::copy(r.name, r.name + kRecNameChars, name);
    name[kRecNameChars] = '\0';
    ObjectProps p;
    p.name = name;
    p.type = ground_type_tags(r.obj_class, name);
    append_props(pending_, p);
    pending_ += '\n';
    ++stats_.objects;
    ++stats_.transforms;
    ++stats_.structures;
}

void TacviewCore::add_explosion(const double pos[3], double radius, double t) {
    const uint32_t id = kExplosionIdBase + next_explosion_++;
    append_position(pending_, id, pos, Mapping{lon_scale_, lat_scale_});
    pending_ += ",Type=Misc+Explosion,Radius=" + num(radius, 1) + "\n";
    explosions_.push_back({id, t + kExplosionLifeSeconds, frame_no_});
    ++stats_.explosions;
}

void TacviewCore::end_frame(double t, std::string& out, uint8_t series_mask) {
    // Removals first: objects of earlier frames that were not added in this one.
    std::string removals;
    for (size_t i = 0; i < objs_.size();) {
        if (objs_[i].seen != frame_no_ && (objs_[i].series & series_mask)) {
            append_removal(removals, objs_[i].id);
            ++stats_.removals;
            objs_[i] = objs_.back();
            objs_.pop_back();
        } else {
            ++i;
        }
    }
    for (const uint32_t id : dead_statics_) {
        append_removal(removals, id);
        ++stats_.removals;
    }
    dead_statics_.clear();
    if ((series_mask & TacviewRec::kSeriesStructure) && !statics_.empty()) {
        size_t kept = 0;
        for (const Static& st : statics_) {
            if (st.seen == frame_no_) {
                statics_[kept++] = st;
                continue;
            }
            append_removal(removals, st.id);
            ++stats_.removals;
        }
        statics_.resize(kept);
        static_index_.clear();
        for (size_t i = 0; i < statics_.size(); ++i) static_index_.emplace(statics_[i].seq, i);
    }
    ++frame_no_;
    // Explosions that are due, or all older ones when the frame lists every series (the last frame).
    for (size_t i = 0; i < explosions_.size();) {
        const bool last = series_mask == TacviewRec::kSeriesAll && explosions_[i].frame < frame_no_ - 1;
        if (last || explosions_[i].remove_at <= t) {
            append_removal(removals, explosions_[i].id);
            explosions_.erase(explosions_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (removals.empty() && pending_.empty() && events_.empty()) return;
    if (!std::isfinite(t)) t = last_t_;
    if (!have_t0_) {
        have_t0_ = true;
        t0_ = t;
        last_t_ = t;
    }
    t = std::max(t, last_t_);
    last_t_ = t;
    const long long ms = std::max(last_ms_, std::llround((t - t0_) * 1000.0));
    if (ms != last_ms_) {
        append_frame(out, ms);
        last_ms_ = ms;
    }
    out += events_;
    out += removals;
    out += pending_;
    events_.clear();
    pending_.clear();
}

}  // namespace LazyWorks::SF2Patch::acmi
