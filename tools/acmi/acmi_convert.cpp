#include "acmi/acmi_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "net/protocol.h"

namespace lazysf2::acmi {
namespace {

using net::StreamKind;

constexpr double kMetresPerDegree = 111320.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;
constexpr uint32_t kHostId = 1;
constexpr uint32_t kClientIdBase = 0x100;

enum class EvType { Join, Sample, Leave };

struct Event {
    double t = 0;
    EvType type = EvType::Sample;
    uint32_t id = 0;     // ACMI object id
    std::string name;    // Join: aircraft
    std::string pilot;   // Join: callsign
    double pos[3] = {};  // Sample
    float ang[3] = {};   // Sample: pitch, heading, roll (radians)
};

std::string fixed_str(const char* s, size_t cap) {
    std::string r(s, strnlen(s, cap));
    for (char& c : r)
        if (c < 0x20 || c > 0x7e) c = '?';
    return r;
}

// A comma inside a text value is escaped with a backslash (spec, "Escaping"). fixed_str removed any newline.
std::string esc(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == ',') r += '\\';
        r += c;
    }
    return r;
}

bool finite_all(const double* p, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

double norm_deg(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0) d += 360.0;
    return d;
}

// Fixed decimals with trailing zeros trimmed (smaller files).
std::string num(double v, int decimals) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", decimals, v);
    std::string r = b;
    if (r.find('.') != std::string::npos) {
        while (!r.empty() && r.back() == '0') r.pop_back();
        if (!r.empty() && r.back() == '.') r.pop_back();
    }
    if (r == "-0" || r.empty()) r = "0";
    return r;
}

struct Presence {
    bool active = false;
    double join_t = 0;
};

struct Obj {
    bool alive = false;
    bool described = false;
    double last_t = 0;
    std::string name, pilot;
};

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

    // Pass 1, in file order (which is host order): decode, track which clients are present, build events.
    std::vector<Event> ev;
    std::vector<Presence> clients(65536);  // indexed by the 16-bit client id

    net::StreamRecord rec;
    net::StreamStatus st;
    while ((st = reader.next(rec)) == net::StreamStatus::Ok) {
        ++info.records;
        if (!std::isfinite(rec.time)) {
            ++info.skipped_bad;
            continue;
        }
        switch (rec.kind) {
            case static_cast<uint8_t>(StreamKind::HostState):
            case static_cast<uint8_t>(StreamKind::StateReceived): {
                net::StateMsg s{};
                if (net::decode(rec.payload, rec.payload_len, s) != net::Status::Ok) {
                    ++info.skipped_bad;
                    break;
                }
                const bool host = rec.kind == static_cast<uint8_t>(StreamKind::HostState);
                double t = host ? rec.time : s.game_time;  // client samples carry the host-clock stamp
                const double ang[3] = {s.ang[0], s.ang[1], s.ang[2]};
                if (!std::isfinite(t) || !finite_all(s.pos, 3) || !finite_all(ang, 3)) {
                    ++info.skipped_bad;
                    break;
                }
                Event e;
                e.type = EvType::Sample;
                if (host) {
                    e.id = kHostId;
                } else {
                    const Presence& c = clients[s.client_id];
                    if (!c.active) {  // never joined in this file
                        ++info.skipped_bad;
                        break;
                    }
                    e.id = kClientIdBase + s.client_id;
                    t = std::max(t, c.join_t);  // a sample cannot come before its object exists
                }
                e.t = t;
                std::memcpy(e.pos, s.pos, sizeof(e.pos));
                std::memcpy(e.ang, s.ang, sizeof(e.ang));
                ev.push_back(std::move(e));
                ++info.samples_in;
                break;
            }
            case static_cast<uint8_t>(StreamKind::ClientJoined): {
                net::ClientJoinedPayload p;
                if (rec.payload_len != sizeof(p)) break;
                std::memcpy(&p, rec.payload, sizeof(p));
                clients[p.client_id] = {true, rec.time};
                Event e;
                e.t = rec.time;
                e.type = EvType::Join;
                e.id = kClientIdBase + p.client_id;
                e.pilot = fixed_str(p.callsign, sizeof(p.callsign));
                e.name = fixed_str(p.aircraft, sizeof(p.aircraft));
                ev.push_back(std::move(e));
                break;
            }
            case static_cast<uint8_t>(StreamKind::ClientLeft): {
                net::ClientLeftPayload p;
                if (rec.payload_len != sizeof(p)) break;
                std::memcpy(&p, rec.payload, sizeof(p));
                clients[p.client_id].active = false;
                Event e;
                e.t = rec.time;
                e.type = EvType::Leave;
                e.id = kClientIdBase + p.client_id;
                ev.push_back(std::move(e));
                break;
            }
            default:
                break;  // Welcome, Bye and unknown kinds carry nothing we plot
        }
    }
    info.end = st;

    // Output frames must not go back in time. The two State kinds interleave out of order, so sort. The sort is
    // stable: a Join stays before the samples clamped to its time, and a Leave after the samples before it.
    std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.t < b.t; });

    const double t0 = ev.empty() ? 0.0 : ev.front().t;
    out += "FileType=text/acmi/tacview\nFileVersion=2.2\n";
    // ReferenceLongitude/Latitude are added to each object's Longitude/Latitude (spec, "Global properties"),
    // so the T= values below are offsets from the reference point.
    out += "0,ReferenceTime=2000-01-01T00:00:00Z,ReferenceLongitude=" + num(opt.ref_lon, 9) +
           ",ReferenceLatitude=" + num(opt.ref_lat, 9) + ",Title=Strike Fighters 2 host stream\n";

    const double lon_scale = 1.0 / (kMetresPerDegree * std::cos(opt.ref_lat / kRadToDeg));
    const double lat_scale = 1.0 / kMetresPerDegree;
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
        char b[48];
        std::snprintf(b, sizeof(b), "#%lld.%03lld\n", ms / 1000, ms % 1000);
        out += b;
    };

    for (const Event& e : ev) {
        char idbuf[16];
        std::snprintf(idbuf, sizeof(idbuf), "%x", e.id);
        Obj& o = find(e.id);
        if (e.type == EvType::Join) {
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
            out += "-";
            out += idbuf;
            out += "\n";
        } else {
            if (e.id == kHostId && !o.alive && !o.described) {
                o.alive = true;
                o.pilot = "Host";
                ++info.objects;
            }
            if (!o.alive) continue;
            if (o.described && e.t - o.last_t < min_dt) continue;  // rate limit
            o.last_t = e.t;
            frame(e.t);
            // T=Lon|Lat|Alt|Roll|Pitch|Yaw|U|V|Heading (spec, "Object transform"). Roll is positive right wing
            // down, pitch positive nose-up and yaw clockwise from north, as in SF2. Yaw and heading are equal
            // because the flat world is aligned with north.
            const double heading = norm_deg(e.ang[1] * kRadToDeg);
            out += idbuf;
            out += ",T=" + num(e.pos[0] * lon_scale, 9) + "|" + num(e.pos[1] * lat_scale, 9) + "|" +
                   num(e.pos[2], 2) + "|" + num(e.ang[2] * kRadToDeg, 2) + "|" + num(e.ang[0] * kRadToDeg, 2) +
                   "|" + num(heading, 2) + "|" + num(e.pos[0], 2) + "|" + num(e.pos[1], 2) + "|" + num(heading, 2);
            if (!o.described) {
                o.described = true;
                const bool host = e.id == kHostId;
                if (!o.name.empty()) out += ",Name=" + esc(o.name);
                out += ",Pilot=" + esc(o.pilot);
                out += ",Type=Air+FixedWing";
                out += host ? ",Coalition=Allies,Color=Blue" : ",Coalition=Enemies,Color=Red";
            }
            out += "\n";
            ++info.samples_out;
        }
    }
    return true;
}

}  // namespace lazysf2::acmi
