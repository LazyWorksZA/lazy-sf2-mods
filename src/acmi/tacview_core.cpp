#include "acmi/tacview_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "acmi/acmi_format.h"

namespace lazysf2::acmi {

namespace {
bool finite3(const double* p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
bool finite3(const float* p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
}  // namespace

TacviewCore::TacviewCore(const TacviewCoreConfig& cfg) : cfg_(cfg) {
    cfg_.ref_lat = std::clamp(cfg_.ref_lat, -89.0, 89.0);
    cfg_.ref_lon = std::clamp(cfg_.ref_lon, -180.0, 180.0);
    const Mapping m = make_mapping(cfg_.ref_lat);
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
    // An aircraft and a weapon can have the same seq (each has its own counter): the class is part of the identity.
    const bool weapon = r.obj_class != TacviewRec::kAircraft;
    State* s = nullptr;
    for (State& o : objs_) {
        if (o.seq == r.seq && (o.obj_class != TacviewRec::kAircraft) == weapon) {
            s = &o;
            break;
        }
    }
    if (r.destroyed) {
        if (s && !s->destroyed) {
            s->destroyed = true;
            append_event_destroyed(events_, s->id);
            ++stats_.destroyed_events;
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
        const uint32_t id = weapon ? kWeaponIdBase + static_cast<uint32_t>(r.seq)
                                   : (r.is_player ? kPlayerId : kOtherIdBase + static_cast<uint32_t>(r.seq));
        objs_.push_back({r.seq, r.obj_class, r.side, false, id, -1e300, 0});
        s = &objs_.back();
    }
    s->seen = frame_no_;  // present in this frame, even if its transform is skipped below
    if (!is_new && r.t - s->last_t < (weapon ? min_dt_weapon_ : min_dt_) - 1e-9) {
        ++stats_.rate_limited;
        return;
    }
    s->last_t = r.t;

    const Mapping m{lon_scale_, lat_scale_};
    append_transform(pending_, s->id, r.pos, r.ang, m);
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
        if (weapon) {
            p.type = r.obj_class == TacviewRec::kBomb     ? "Weapon+Bomb"
                     : r.obj_class == TacviewRec::kRocket ? "Weapon+Rocket"
                                                           : "Weapon+Missile";
            if (r.parent_seq != 0) {
                uint32_t pid = kOtherIdBase + static_cast<uint32_t>(r.parent_seq);  // an aircraft we have not seen
                for (const State& o : objs_) {
                    if (o.seq == r.parent_seq && o.obj_class == TacviewRec::kAircraft) {
                        pid = o.id;
                        if (side == hooks::AircraftSide::Unknown) side = static_cast<hooks::AircraftSide>(o.side);
                        break;
                    }
                }
                std::snprintf(parent_id, sizeof(parent_id), "%x", pid);
                p.parent = parent_id;
            }
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

void TacviewCore::end_frame(double t, std::string& out) {
    // Removals first: objects of earlier frames that were not added in this one.
    std::string removals;
    for (size_t i = 0; i < objs_.size();) {
        if (objs_[i].seen != frame_no_) {
            append_removal(removals, objs_[i].id);
            ++stats_.removals;
            objs_[i] = objs_.back();
            objs_.pop_back();
        } else {
            ++i;
        }
    }
    ++frame_no_;
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

}  // namespace lazysf2::acmi
