#include "acmi/net_report.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hooks/replay_math.h"
#include "net/protocol.h"
#include "net/remote_jet.h"
#include "net/stream_format.h"

namespace LazyWorks::SF2Patch::acmi {
namespace {

namespace rm = hooks::replay_math;
using net::StreamKind;

constexpr double kMaxTruthGap = 1.0;          // seconds between truth samples: wider is a gap, not compared
constexpr double kAccelMinDt = 0.05, kAccelMaxDt = 0.5;  // spacing of the shown records the smoothness figure uses
constexpr double kCoastMaxDt = 1.0;           // a Coast record counts for at most this long
constexpr double kLastRecordDt = 0.1;         // ... and the last record for the 10 Hz cadence
constexpr double kG = 9.80665;
constexpr uint8_t kOutSnap = 1, kOutCoast = 4;  // RemoteJet::Out

struct Shown {
    double t;
    double p[3];
    double a[3];
    uint8_t out;
};

struct File {
    bool have_session = false;
    net::SessionInfoPayload session{};
    std::vector<net::StreamRecord> recs;
    bool truncated = false;  // the file ends in the middle of a record
};

bool read_file(const uint8_t* data, size_t size, File& f, std::string& err, const char* which) {
    net::StreamReader rd(data, size);
    net::StreamHeader h{};
    const net::StreamStatus hs = rd.read_header(h);
    if (hs != net::StreamStatus::Ok) {
        err = std::string("the ") + which + " file is not a valid stream (" + net::stream_status_name(hs) + ")";
        return false;
    }
    net::StreamRecord rec;
    net::StreamStatus st;
    while ((st = rd.next(rec)) == net::StreamStatus::Ok) {
        if (!std::isfinite(rec.time)) continue;
        if (const uint8_t pv = net::record_protocol_version(rec); pv != 0 && pv != net::kVersion) {
            err = std::string("the ") + which + " file was recorded with protocol v" + std::to_string(pv) +
                  "; this build reads v" + std::to_string(net::kVersion) + " (record it again)";
            return false;
        }
        if (rec.kind == static_cast<uint8_t>(StreamKind::SessionInfo) && !f.have_session &&
            rec.payload_len == sizeof(f.session)) {
            std::memcpy(&f.session, rec.payload, sizeof(f.session));
            f.have_session = true;
        }
        f.recs.push_back(rec);
    }
    f.truncated = st == net::StreamStatus::Truncated;
    return true;  // a truncated tail keeps what came before it
}

std::string label(const File& f) {
    if (!f.have_session) return "no SessionInfo";
    const net::SessionInfoPayload& s = f.session;
    const char* role = s.role == 0 ? "host" : s.role == 1 ? "client" : s.role == 2 ? "bot serve" : "role ?";
    char cs[17] = {}, ac[33] = {};
    std::memcpy(cs, s.callsign, 16);
    std::memcpy(ac, s.aircraft, 32);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %u \"%s\" (%s)", role, static_cast<unsigned>(s.own_id), cs, ac);
    return buf;
}

rm::Samp to_samp(const net::StateMsg& s) {
    rm::Samp o{};
    o.t = s.game_time;
    for (int i = 0; i < 3; ++i) {
        o.k.p[i] = s.pos[i];
        o.k.v[i] = s.vel[i];
        o.k.a[i] = s.ang[i];
        o.k.w[i] = s.rates[i];
    }
    return o;
}

/// <summary>The truth's States of one kind and id, by their game_time, sorted, one per time.</summary>
std::vector<rm::Samp> truth_samples(const File& f, StreamKind kind, int id) {
    std::vector<rm::Samp> out;
    for (const net::StreamRecord& rec : f.recs) {
        if (rec.kind != static_cast<uint8_t>(kind)) continue;
        net::StateMsg s{};
        if (net::decode(rec.payload, rec.payload_len, s) != net::Status::Ok) continue;
        if (kind != StreamKind::HostState && s.client_id != id) continue;
        if (!std::isfinite(s.game_time)) continue;
        out.push_back(to_samp(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const rm::Samp& a, const rm::Samp& b) { return a.t < b.t; });
    out.erase(std::unique(out.begin(), out.end(), [](const rm::Samp& a, const rm::Samp& b) { return !(b.t > a.t); }),
              out.end());
    return out;
}

/// <summary>The formation point (net::join_position's frame): right (cos h, -sin h), ahead (sin h, cos h).</summary>
void apply_offset(rm::Kin& k, double heading_rate, const double off[3]) {
    const double h = k.a[1], sh = std::sin(h), ch = std::cos(h);
    k.p[0] += off[0] * ch - off[1] * sh;
    k.p[1] += -off[0] * sh - off[1] * ch;
    k.p[2] += off[2];
    k.v[0] += heading_rate * (-off[0] * sh - off[1] * ch);
    k.v[1] += heading_rate * (-off[0] * ch + off[1] * sh);
}

enum class At { Ok, Outside, Gap };

At truth_at(const std::vector<rm::Samp>& s, double t, const ReportOptions& opt, rm::Kin& k) {
    if (s.empty() || t < s.front().t || t > s.back().t) return At::Outside;
    size_t hi = static_cast<size_t>(
        std::upper_bound(s.begin(), s.end(), t, [](double v, const rm::Samp& x) { return v < x.t; }) - s.begin());
    if (hi >= s.size()) hi = s.size() - 1;
    const size_t lo = hi > 0 ? hi - 1 : 0;
    if (s[hi].t - s[lo].t > kMaxTruthGap) return At::Gap;
    rm::interpolate(s[lo], s[hi], t, &k);
    if (opt.has_offset) {
        const double dt = s[hi].t - s[lo].t;
        const double hr = dt > 1e-9 ? rm::angle_diff(s[hi].k.a[1], s[lo].k.a[1]) / dt : 0.0;
        apply_offset(k, hr, opt.offset);
    }
    return At::Ok;
}

double pct(std::vector<double>& v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::ceil(q * static_cast<double>(v.size())));
    i = i == 0 ? 0 : i - 1;
    return v[std::min(i, v.size() - 1)];
}

ErrStat stat_mag(std::vector<double> v) {
    ErrStat e;
    e.n = v.size();
    if (v.empty()) return e;
    e.median = pct(v, 0.5);
    e.p95 = pct(v, 0.95);
    e.max = v.back();
    return e;
}

ErrStat stat_signed(std::vector<double> v) {
    ErrStat e;
    e.n = v.size();
    if (v.empty()) return e;
    e.median = pct(v, 0.5);
    for (double& x : v) x = std::fabs(x);
    e.p95 = pct(v, 0.95);
    e.max = v.back();
    return e;
}

struct Acc {
    double t0 = 0, t1 = 0;
    bool any = false;
    std::vector<double> pos, along, cross, vert, ang, accel;
    unsigned snaps = 0;
    double coast_s = 0, truth_accel_max = 0;
    void span(double t) {
        if (!any) t0 = t1 = t;
        t1 = std::max(t1, t);
        any = true;
    }
    ReportRow row() const {
        ReportRow r;
        r.t0 = t0;
        r.t1 = t1;
        r.pos = stat_mag(pos);
        r.along = stat_signed(along);
        r.cross = stat_signed(cross);
        r.vert = stat_signed(vert);
        r.ang = stat_mag(ang);
        r.snaps = snaps;
        r.coast_s = coast_s;
        std::vector<double> a = accel;
        r.accel_p95_g = pct(a, 0.95);
        r.accel_max_g = a.empty() ? 0 : a.back();
        r.truth_accel_max_g = truth_accel_max;
        return r;
    }
};

double accel_g(const double p0[3], const double p1[3], const double p2[3], double dt1, double dt2) {
    double s = 0;
    for (int i = 0; i < 3; ++i) {
        const double a = 2.0 * ((p2[i] - p1[i]) / dt2 - (p1[i] - p0[i]) / dt1) / (dt1 + dt2);
        s += a * a;
    }
    return std::sqrt(s) / kG;
}

}  // namespace

bool make_report(const uint8_t* vdata, size_t vsize, const uint8_t* tdata, size_t tsize, const ReportOptions& opt,
                 Report& r) {
    r = Report{};
    File vf, tf;
    if (!read_file(vdata, vsize, vf, r.error, "viewer") || !read_file(tdata, tsize, tf, r.error, "truth")) return false;
    r.viewer_truncated = vf.truncated;
    r.truth_truncated = tf.truncated;
    r.viewer_label = label(vf);
    r.truth_label = label(tf);

    // The remote's id: given, or the lowest one the viewer showed.
    int id = opt.id;
    if (id < 0) {
        for (const net::StreamRecord& rec : vf.recs) {
            if (rec.kind != static_cast<uint8_t>(StreamKind::RemoteShown) || rec.payload_len != sizeof(net::RemoteShownPayload))
                continue;
            net::RemoteShownPayload p;
            std::memcpy(&p, rec.payload, sizeof(p));
            if (id < 0 || p.id < id) id = p.id;
        }
        if (id < 0) {
            r.error = "the viewer file has no RemoteShown records";
            return false;
        }
    }
    r.id = id;

    std::vector<Shown> shown;
    for (const net::StreamRecord& rec : vf.recs) {
        if (rec.kind == static_cast<uint8_t>(StreamKind::AvatarEvent) && rec.payload_len == sizeof(net::AvatarEventPayload)) {
            net::AvatarEventPayload p;
            std::memcpy(&p, rec.payload, sizeof(p));
            if (p.id != id) continue;
            if (p.event == static_cast<uint8_t>(net::AvatarEventType::Parked)) ++r.parked;
            if (p.event == static_cast<uint8_t>(net::AvatarEventType::Taken)) ++r.taken;
            if (p.event == static_cast<uint8_t>(net::AvatarEventType::Lost)) ++r.lost;
            continue;
        }
        if (rec.kind != static_cast<uint8_t>(StreamKind::RemoteShown) || rec.payload_len != sizeof(net::RemoteShownPayload))
            continue;
        net::RemoteShownPayload p;
        std::memcpy(&p, rec.payload, sizeof(p));
        if (p.id != id) continue;
        Shown s{rec.time, {p.pos[0], p.pos[1], p.pos[2]}, {p.ang[0], p.ang[1], p.ang[2]}, p.out};
        bool finite = true;
        for (int i = 0; i < 3; ++i) finite = finite && std::isfinite(s.p[i]) && std::isfinite(s.a[i]);
        if (finite) shown.push_back(s);
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) { return a.t < b.t; });
    r.shown = shown.size();
    if (shown.empty()) {
        char b[96];
        std::snprintf(b, sizeof(b), "the viewer file has no RemoteShown records of id %d", id);
        r.error = b;
        return false;
    }

    // The truth: the lead plus the offset, or the jet's own States (as sent, then as the host had them).
    std::vector<rm::Samp> truth;
    if (opt.has_offset) {
        truth = truth_samples(tf, StreamKind::HostState, 0);
        char b[128];
        std::snprintf(b, sizeof(b), "HostState of the truth file + offset %.0f m right, %.0f m back, %.0f m up", opt.offset[0],
                      opt.offset[1], opt.offset[2]);
        r.truth_source = b;
    } else {
        // The host's jet at every step beats its 20 Hz States as sent.
        if (id == 0) {
            truth = truth_samples(tf, StreamKind::HostState, 0);
            r.truth_source = "HostState (the host's own jet, every step)";
        }
        if (truth.empty()) {
            truth = truth_samples(tf, StreamKind::LocalState, id);
            r.truth_source = "LocalState (the jet's own States as sent)";
        }
        if (truth.empty()) {
            truth = truth_samples(tf, StreamKind::StateReceived, id);
            r.truth_source = "StateReceived (the jet's States as the truth file's recorder received them, by their stamps)";
        }
    }
    if (truth.empty()) {
        char b[128];
        std::snprintf(b, sizeof(b), "the truth file has no States of id %d%s", id, opt.has_offset ? " (HostState, for --offset)" : "");
        r.error = b;
        return false;
    }
    r.viewer_span[0] = shown.front().t;
    r.viewer_span[1] = shown.back().t;
    r.truth_span[0] = truth.front().t;
    r.truth_span[1] = truth.back().t;
    if (r.viewer_span[1] < r.truth_span[0] || r.viewer_span[0] > r.truth_span[1]) {
        char b[192];
        std::snprintf(b, sizeof(b), "the files do not overlap in host time (viewer %.3f..%.3f, truth %.3f..%.3f)",
                      r.viewer_span[0], r.viewer_span[1], r.truth_span[0], r.truth_span[1]);
        r.error = b;
        return false;
    }

    Acc all;
    std::vector<Acc> rows;
    double row0 = -1;
    auto row_of = [&](double t) -> Acc* {
        if (row0 < 0) return nullptr;
        const double w = opt.window > 0 ? opt.window : 10.0;
        const size_t i = static_cast<size_t>(std::max(0.0, std::floor((t - row0) / w)));
        if (i >= 100000) return nullptr;
        if (rows.size() <= i) rows.resize(i + 1);
        return &rows[i];
    };

    // Truth positions at the shown times, for the smoothness figure of both.
    std::vector<char> have_truth(shown.size(), 0);
    std::vector<rm::Kin> tk(shown.size());
    for (size_t i = 0; i < shown.size(); ++i) {
        const Shown& s = shown[i];
        const bool coast = s.out == kOutCoast;
        if (coast) {
            const double dt = i + 1 < shown.size() ? std::min(shown[i + 1].t - s.t, kCoastMaxDt) : kLastRecordDt;
            all.coast_s += dt;
            if (Acc* a = row_of(s.t)) a->coast_s += dt;
            continue;
        }
        rm::Kin k{};
        const At at = truth_at(truth, s.t, opt, k);
        if (at == At::Outside) {
            ++r.outside;
            continue;
        }
        if (at == At::Gap) {
            ++r.gaps;
            continue;
        }
        if (row0 < 0) row0 = s.t;
        have_truth[i] = 1;
        tk[i] = k;
        ++r.compared;
        double e[3];
        for (int j = 0; j < 3; ++j) e[j] = s.p[j] - k.p[j];
        double th[2] = {k.v[0], k.v[1]};
        const double hs = std::hypot(th[0], th[1]);
        if (hs > 1.0) {
            th[0] /= hs;
            th[1] /= hs;
        } else {
            th[0] = std::sin(k.a[1]);
            th[1] = std::cos(k.a[1]);
        }
        const double along = e[0] * th[0] + e[1] * th[1];
        const double cross = e[0] * th[1] - e[1] * th[0];  // right of the track: (th.y, -th.x)
        const double ang = net::jet_math::rotation_distance(s.a, k.a) * 180.0 / rm::kPi;
        const double pos = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        for (Acc* a : {&all, row_of(s.t)}) {
            if (!a) continue;
            a->span(s.t);
            a->pos.push_back(pos);
            a->along.push_back(along);
            a->cross.push_back(cross);
            a->vert.push_back(e[2]);
            a->ang.push_back(ang);
            if (s.out == kOutSnap) ++a->snaps;
        }
    }
    // Snaps outside the compared span still count overall.
    for (size_t i = 0; i < shown.size(); ++i)
        if (shown[i].out == kOutSnap && !have_truth[i]) ++all.snaps;
    for (size_t i = 1; i + 1 < shown.size(); ++i) {
        // Coasting records hold a stale place, and a snap is a jump counted on its own.
        bool skip = false;
        for (size_t j = i - 1; j <= i + 1; ++j) skip = skip || shown[j].out == kOutCoast || shown[j].out == kOutSnap;
        if (skip) continue;
        const double d1 = shown[i].t - shown[i - 1].t, d2 = shown[i + 1].t - shown[i].t;
        if (d1 < kAccelMinDt || d2 < kAccelMinDt || d1 > kAccelMaxDt || d2 > kAccelMaxDt) continue;
        const double g = accel_g(shown[i - 1].p, shown[i].p, shown[i + 1].p, d1, d2);
        Acc* rw = have_truth[i] ? row_of(shown[i].t) : nullptr;
        all.accel.push_back(g);
        if (rw) rw->accel.push_back(g);
        if (have_truth[i - 1] && have_truth[i] && have_truth[i + 1]) {
            const double gt = accel_g(tk[i - 1].p, tk[i].p, tk[i + 1].p, d1, d2);
            all.truth_accel_max = std::max(all.truth_accel_max, gt);
            if (rw) rw->truth_accel_max = std::max(rw->truth_accel_max, gt);
        }
    }
    for (const Acc& a : rows)
        if (a.any || a.coast_s > 0) r.rows.push_back(a.row());
    r.overall = all.row();
    if (r.compared == 0) {
        r.error = "no RemoteShown record of the id falls inside the truth's States (all outside, in gaps, or coasting)";
        return false;
    }
    r.ok = true;
    return true;
}

std::string format_report(const Report& r) {
    std::string out;
    char b[512];
    auto put = [&](const char* fmt, auto... a) {
        std::snprintf(b, sizeof(b), fmt, a...);
        out += b;
    };
    if (!r.ok) {
        put("no report: %s\n", r.error.c_str());
        return out;
    }
    put("viewer: %s; truth: %s\n", r.viewer_label.c_str(), r.truth_label.c_str());
    if (r.viewer_truncated || r.truth_truncated)
        put("warning: the %s file ends in a truncated record (the last moments are missing)\n",
            r.viewer_truncated && r.truth_truncated ? "viewer and truth" : r.viewer_truncated ? "viewer" : "truth");
    put("remote id %d; truth from %s\n", r.id, r.truth_source.c_str());
    put("aligned on host time: viewer %.3f..%.3f, truth %.3f..%.3f\n", r.viewer_span[0], r.viewer_span[1], r.truth_span[0],
        r.truth_span[1]);
    put("%zu shown records: %zu compared, %zu outside the truth's span, %zu in truth gaps over %.0f s\n", r.shown,
        r.compared, r.outside, r.gaps, kMaxTruthGap);
    if (r.outside * 10 > r.shown) put("note: the files overlap only in part\n");
    if (r.parked || r.taken || r.lost) put("avatar events: %u parked, %u taken, %u lost\n", r.parked, r.taken, r.lost);
    out += "metres (along + ahead of the truth on the ground track, cross + right of it, vert + above (world up); signed median, then |p95| |max|), "
           "angle in degrees, accel in g\n";
    out += "  host time          n   pos med/p95/max      along med/p95/max    cross med/p95/max    vert med/p95/max     "
           "ang med/p95/max    snaps coast s  accel p95/max (truth max)\n";
    auto row = [&](const char* name, const ReportRow& w) {
        if (name)
            put("  %-15s", name);
        else
            put("  %7.1f..%-7.1f", w.t0, w.t1);
        put(" %5zu %6.1f %6.1f %6.1f  %6.1f %6.1f %6.1f  %6.1f %6.1f %6.1f  %6.1f %6.1f %6.1f  %5.1f %5.1f %5.1f  %5u %7.1f  "
            "%5.2f %5.2f (%.2f)\n",
            w.pos.n, w.pos.median, w.pos.p95, w.pos.max, w.along.median, w.along.p95, w.along.max, w.cross.median,
            w.cross.p95, w.cross.max, w.vert.median, w.vert.p95, w.vert.max, w.ang.median, w.ang.p95, w.ang.max, w.snaps,
            w.coast_s, w.accel_p95_g, w.accel_max_g, w.truth_accel_max_g);
    };
    for (const ReportRow& w : r.rows) row(nullptr, w);
    row("overall", r.overall);
    return out;
}

}  // namespace LazyWorks::SF2Patch::acmi
