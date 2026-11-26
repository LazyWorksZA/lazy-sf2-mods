#include "acmi/acmi_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "acmi/acmi_format.h"
#include "net/protocol.h"

namespace LazyWorks::SF2Patch::acmi {
namespace {

using net::StreamKind;

constexpr uint32_t kHostId = 1;
constexpr uint32_t kClientIdBase = 0x100;
constexpr uint32_t kGhostIdBase = 0x200;
constexpr uint8_t kOutCoast = 4;  // RemoteJet::Out::Coast

enum class EvType { Join, Sample, Leave };

struct Event {
    double t = 0;
    EvType type = EvType::Sample;
    uint32_t id = 0;     // ACMI object id
    bool lazy = false;   // Sample: creates its object if none is shown (the host, the recorder's own jet, ghosts)
    bool ghost = false;
    std::string name;    // Join, or a lazy Sample: aircraft (ghosts: "<callsign> as seen by <viewer>")
    std::string pilot;   // Join, or a lazy Sample: callsign
    double pos[3] = {};  // Sample
    float ang[3] = {};   // Sample: pitch, heading, roll (radians)
};

std::string fixed_str(const char* s, size_t cap) {
    std::string r(s, strnlen(s, cap));
    for (char& c : r)
        if (c < 0x20 || c > 0x7e) c = '?';
    return r;
}

bool finite_all(const double* p, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

struct Presence {
    bool active = false;
    double join_t = 0;
};

struct Names {
    std::string callsign, aircraft;
};

struct Obj {
    bool alive = false;
    bool described = false;
    bool ghost = false;
    double last_t = 0;
    std::string name, pilot;
};

uint32_t object_id(uint16_t src) { return src == 0 ? kHostId : kClientIdBase + src; }

}  // namespace

bool convert_stream(const uint8_t* data, size_t size, const Options& opt_in, std::string& out, Info& info) {
    info = Info{};
    out.clear();
    Options opt = opt_in;
    opt.ref_lat = std::clamp(opt.ref_lat, -89.0, 89.0);
    opt.ref_lon = std::clamp(opt.ref_lon, -180.0, 180.0);

    net::StreamReader reader(data, size);
    net::StreamHeader hdr{};
    const net::StreamStatus hs = reader.read_header(hdr);
    if (hs != net::StreamStatus::Ok) {
        info.end = hs;
        return false;
    }
    info.header_ok = true;

    // Pass 1, in file order (the recorder's game-thread order): decode, track which jets are present, build events.
    std::vector<Event> ev;
    std::vector<Presence> clients(65536);  // indexed by the 16-bit source id
    std::vector<Names> names(65536);
    bool have_session = false;
    net::SessionInfoPayload session{};
    auto is_role = [&](net::SessionRole r) { return have_session && session.role == static_cast<uint8_t>(r); };
    auto viewer = [&]() -> std::string {
        if (have_session && session.callsign[0]) return fixed_str(session.callsign, sizeof(session.callsign));
        return is_role(net::SessionRole::Client) ? "the client" : "the host";
    };

    net::StreamRecord rec;
    net::StreamStatus st;
    while ((st = reader.next(rec)) == net::StreamStatus::Ok) {
        ++info.records;
        if (!std::isfinite(rec.time)) {
            ++info.skipped_bad;
            continue;
        }
        if (const uint8_t pv = net::record_protocol_version(rec); pv != 0 && pv != net::kVersion) {
            if (!info.other_protocol++) info.other_version = pv;
            continue;
        }
        switch (rec.kind) {
            case static_cast<uint8_t>(StreamKind::HostState):
            case static_cast<uint8_t>(StreamKind::StateReceived):
            case static_cast<uint8_t>(StreamKind::LocalState): {
                net::StateMsg s{};
                if (net::decode(rec.payload, rec.payload_len, s) != net::Status::Ok) {
                    ++info.skipped_bad;
                    break;
                }
                const bool host = rec.kind == static_cast<uint8_t>(StreamKind::HostState);
                const bool local = rec.kind == static_cast<uint8_t>(StreamKind::LocalState);
                const uint16_t src = host ? 0 : s.client_id;
                double t = host ? rec.time : s.game_time;  // other samples carry the sender's host-clock stamp
                const double ang[3] = {s.ang[0], s.ang[1], s.ang[2]};
                if (!std::isfinite(t) || !finite_all(s.pos, 3) || !finite_all(ang, 3)) {
                    ++info.skipped_bad;
                    break;
                }
                Event e;
                e.type = EvType::Sample;
                e.id = object_id(src);
                if (src == 0 || local) {  // the host's jet, or the recorder's own: no join record
                    e.lazy = true;
                    e.name = names[src].aircraft;
                    e.pilot = names[src].callsign.empty() && src == 0 ? "Host" : names[src].callsign;
                } else {
                    const Presence& c = clients[src];
                    if (!c.active) {  // never joined in this file
                        ++info.skipped_bad;
                        break;
                    }
                    t = std::max(t, c.join_t);  // a sample cannot come before its object exists
                }
                e.t = t;
                std::memcpy(e.pos, s.pos, sizeof(e.pos));
                std::memcpy(e.ang, s.ang, sizeof(e.ang));
                ev.push_back(std::move(e));
                ++info.samples_in;
                break;
            }
            case static_cast<uint8_t>(StreamKind::SessionInfo): {
                if (rec.payload_len != sizeof(session) || have_session) break;
                std::memcpy(&session, rec.payload, sizeof(session));
                have_session = true;
                names[session.own_id] = {fixed_str(session.callsign, sizeof(session.callsign)),
                                         fixed_str(session.aircraft, sizeof(session.aircraft))};
                break;
            }
            case static_cast<uint8_t>(StreamKind::ClientJoined):
            case static_cast<uint8_t>(StreamKind::PeerInfoReceived): {
                uint16_t id = 0;
                Names n;
                if (rec.kind == static_cast<uint8_t>(StreamKind::ClientJoined)) {
                    net::ClientJoinedPayload p;
                    if (rec.payload_len != sizeof(p)) break;
                    std::memcpy(&p, rec.payload, sizeof(p));
                    id = p.client_id;
                    n = {fixed_str(p.callsign, sizeof(p.callsign)), fixed_str(p.aircraft, sizeof(p.aircraft))};
                } else {
                    net::PeerInfoMsg p{};
                    if (net::decode(rec.payload, rec.payload_len, p) != net::Status::Ok) break;
                    id = p.id;
                    n = {fixed_str(p.callsign, sizeof(p.callsign)), fixed_str(p.aircraft, sizeof(p.aircraft))};
                    // A re-announcement (after a re-Welcome) of a jet already shown is not a new aircraft.
                    if (id != 0 && clients[id].active) {
                        names[id] = n;
                        break;
                    }
                }
                names[id] = n;
                if (id == 0) break;  // the host's jet has no join
                clients[id] = {true, rec.time};
                Event e;
                e.t = rec.time;
                e.type = EvType::Join;
                e.id = object_id(id);
                e.pilot = n.callsign;
                e.name = n.aircraft;
                ev.push_back(std::move(e));
                break;
            }
            case static_cast<uint8_t>(StreamKind::ClientLeft): {
                net::ClientLeftPayload p;
                if (rec.payload_len != sizeof(p)) break;
                std::memcpy(&p, rec.payload, sizeof(p));
                if (p.client_id == 0) break;
                clients[p.client_id].active = false;
                Event e;
                e.t = rec.time;
                e.type = EvType::Leave;
                e.id = object_id(p.client_id);
                ev.push_back(e);
                if (opt.ghosts) {
                    e.id = kGhostIdBase + p.client_id;
                    ev.push_back(e);
                }
                break;
            }
            case static_cast<uint8_t>(StreamKind::RemoteShown): {
                if (!opt.ghosts) break;
                net::RemoteShownPayload p;
                if (rec.payload_len != sizeof(p)) break;
                std::memcpy(&p, rec.payload, sizeof(p));
                if (p.out == kOutCoast) break;  // the last shown place, held: the aircraft itself coasts on
                const double ang[3] = {p.ang[0], p.ang[1], p.ang[2]};
                if (!finite_all(p.pos, 3) || !finite_all(ang, 3)) {
                    ++info.skipped_bad;
                    break;
                }
                Event e;
                e.t = rec.time;
                e.type = EvType::Sample;
                e.id = kGhostIdBase + p.id;
                e.lazy = e.ghost = true;
                const std::string& cs = names[p.id].callsign;
                e.name = (cs.empty() ? (p.id == 0 ? std::string("Host") : "id " + std::to_string(p.id)) : cs) +
                         " as seen by " + viewer();
                std::memcpy(e.pos, p.pos, sizeof(e.pos));
                std::memcpy(e.ang, p.ang, sizeof(e.ang));
                ev.push_back(std::move(e));
                break;
            }
            default:
                break;  // Welcome, Bye, avatar events and unknown kinds carry nothing we plot
        }
    }
    info.end = st;

    // Output frames must not go back in time. The kinds interleave out of order, so sort. The sort is stable: a Join
    // stays before the samples clamped to its time, and a Leave after the samples before it.
    std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.t < b.t; });

    const double t0 = ev.empty() ? 0.0 : ev.front().t;
    const char* title = is_role(net::SessionRole::Client)     ? "Strike Fighters 2 client stream"
                        : is_role(net::SessionRole::BotServe) ? "Strike Fighters 2 bot serve stream"
                                                              : "Strike Fighters 2 host stream";
    append_header(out, "2000-01-01T00:00:00Z", opt.ref_lon, opt.ref_lat, title);

    const Mapping map = make_mapping(opt.ref_lat);
    const double min_dt = opt.max_rate > 0 ? 1.0 / opt.max_rate : 0.0;

    std::vector<std::pair<uint32_t, Obj>> objs;
    auto find = [&](uint32_t id) -> Obj& {
        for (auto& o : objs)
            if (o.first == id) return o.second;
        objs.emplace_back(id, Obj{});
        return objs.back().second;
    };
    long long last_frame = -1;
    auto frame = [&](double t) {
        const long long ms = std::llround((t - t0) * 1000.0);
        if (ms == last_frame) return;
        last_frame = ms;
        append_frame(out, ms);
    };

    for (const Event& e : ev) {
        Obj& o = find(e.id);
        if (e.type == EvType::Join) {
            if (o.alive && o.described) {  // the old object is still shown (its Leave was lost): remove it first
                frame(e.t);
                append_removal(out, e.id);
            }
            o = Obj{};  // a new aircraft in a reused slot starts afresh
            o.alive = true;
            o.name = e.name;
            o.pilot = e.pilot;
            ++info.objects;
        } else if (e.type == EvType::Leave) {
            if (!o.alive) continue;
            o.alive = false;
            if (!o.described) continue;  // never shown, nothing to remove
            frame(e.t);
            append_removal(out, e.id);
        } else {
            if (e.lazy && !o.alive) {
                o = Obj{};
                o.alive = true;
                o.ghost = e.ghost;
                o.name = e.name;
                o.pilot = e.pilot;
                ++info.objects;
            }
            if (!o.alive) continue;
            if (o.described && e.t - o.last_t < min_dt - 1e-9) continue;  // rate limit
            // Step the limiter's clock by exactly min_dt (not to e.t), so that an input rate just above the cap is
            // thinned to the cap rather than to a lower one; after a gap, restart from this sample.
            o.last_t = (o.described && e.t - o.last_t < 2.0 * min_dt) ? o.last_t + min_dt : e.t;
            frame(e.t);
            // Roll is positive right wing down, pitch positive nose-up and yaw clockwise from north, as in SF2
            // (the transform's layout is documented at append_transform).
            const double pos[3] = {e.pos[0], e.pos[1], e.pos[2]};
            append_transform(out, e.id, pos, e.ang, map);
            if (!o.described) {
                o.described = true;
                const bool host = e.id == kHostId;
                ObjectProps pr;
                pr.name = o.name.c_str();
                pr.pilot = o.pilot.c_str();
                pr.type = "Air+FixedWing";
                pr.coalition = o.ghost ? "Neutrals" : host ? "Allies" : "Enemies";
                pr.color = o.ghost ? "Green" : host ? "Blue" : "Red";
                append_props(out, pr);
            }
            out += '\n';
            ++info.samples_out;
            if (o.ghost) ++info.ghost_samples;
        }
    }
    return true;
}

}  // namespace LazyWorks::SF2Patch::acmi
