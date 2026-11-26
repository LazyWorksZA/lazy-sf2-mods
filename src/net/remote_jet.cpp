// RemoteJet: see remote_jet.h and the Phase 4 plan, section 2.5.
#include "net/remote_jet.h"

#include <cmath>
#include <cstring>

namespace LazyWorks::SF2Patch::net {

namespace jet_math {

Quat q_mul(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
Quat q_conj(const Quat& a) { return {a.w, -a.x, -a.y, -a.z}; }

namespace {
Quat normalized(Quat q) {
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!(n > 1e-12)) return {1, 0, 0, 0};
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}
}  // namespace

Quat q_from_euler(const double e[3]) {
    // Columns right, forward, up of the body-to-world matrix, as replay_math::build_matrix builds its rows.
    const double cp = std::cos(e[0]), sp = std::sin(e[0]), ch = std::cos(e[1]), sh = std::sin(e[1]);
    const double cr = std::cos(e[2]), sr = std::sin(e[2]);
    const double f[3] = {sh * cp, ch * cp, sp}, r0[3] = {ch, -sh, 0.0}, u0[3] = {-sh * sp, -ch * sp, cp};
    double r[3], u[3];
    for (int i = 0; i < 3; ++i) {
        r[i] = r0[i] * cr - u0[i] * sr;
        u[i] = u0[i] * cr + r0[i] * sr;
    }
    const double m00 = r[0], m01 = f[0], m02 = u[0], m10 = r[1], m11 = f[1], m12 = u[1], m20 = r[2], m21 = f[2],
                 m22 = u[2];
    const double tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        q = {0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
    }
    return normalized(q);
}

void q_to_euler(const Quat& q, double e[3]) {
    const double r[3] = {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.z * q.w), 2 * (q.x * q.z - q.y * q.w)};
    const double f[3] = {2 * (q.x * q.y - q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.x * q.w)};
    const double u2 = 1 - 2 * (q.x * q.x + q.y * q.y);
    e[0] = std::asin(replay_math::clampd(f[2], -1.0, 1.0));
    if (std::sqrt(f[0] * f[0] + f[1] * f[1]) > 1e-9) {
        e[1] = std::atan2(f[0], f[1]);
        e[2] = std::atan2(-r[2], u2);
    } else {  // straight up or down: only heading + roll is defined
        e[1] = std::atan2(-r[1], r[0]);
        e[2] = 0.0;
    }
    e[1] = replay_math::wrap_pi(e[1]);
    e[2] = replay_math::wrap_pi(e[2]);
}

void q_log(const Quat& qi, double r[3]) {
    const Quat q = qi.w < 0 ? Quat{-qi.w, -qi.x, -qi.y, -qi.z} : qi;
    const double v = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    const double k = v > 1e-12 ? 2.0 * std::atan2(v, q.w) / v : 2.0;
    r[0] = q.x * k;
    r[1] = q.y * k;
    r[2] = q.z * k;
}

Quat q_exp(const double r[3]) {
    const double th = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(th > 1e-12)) return normalized({1.0, 0.5 * r[0], 0.5 * r[1], 0.5 * r[2]});
    const double s = std::sin(0.5 * th) / th;
    return {std::cos(0.5 * th), r[0] * s, r[1] * s, r[2] * s};
}

double q_angle(const Quat& a, const Quat& b) {
    const Quat d = q_mul(a, q_conj(b));
    return 2.0 * std::atan2(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), std::fabs(d.w));
}

double rotation_distance(const double ea[3], const double eb[3]) { return q_angle(q_from_euler(ea), q_from_euler(eb)); }

}  // namespace jet_math

using namespace jet_math;

namespace {
double norm3(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
double dist3(const double a[3], const double b[3]) {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return norm3(d);
}
// b turned from a by the fraction s of the rotation between them.
Quat q_slerp(const Quat& a, const Quat& b, double s) {
    double r[3];
    q_log(q_mul(b, q_conj(a)), r);
    for (double& x : r) x *= s;
    return q_mul(q_exp(r), a);
}
}  // namespace

bool RemoteJet::push(const StateMsg& s, double now) {
    if (!finite(now) || !finite(s.game_time)) return false;
    touch(now);
    if (have_seq_) {
        const bool any = n_ + pn_ > 0;
        const int32_t ds = static_cast<int32_t>(s.seq - last_seq_);
        const bool newer_t = !any || s.game_time > newest_t_;
        if (ds <= -kSeqRestart || (any && newest_t_ - s.game_time > p_.restart_back) || (ds <= 0 && newer_t)) {
            restart();
        } else if (ds <= 0 || !newer_t) {
            ++st_.dup_or_old;
            return false;
        }
    }
    const bool any = n_ + pn_ > 0;
    if (any && s.game_time - newest_t_ < 2.0) {  // a longer gap is a stall, not a rate
        iv_[iv_pos_] = s.game_time - newest_t_;
        iv_pos_ = (iv_pos_ + 1) % kIntervalWindow;
        if (iv_n_ < kIntervalWindow) ++iv_n_;
    }
    age_[age_pos_] = now - s.game_time;
    age_pos_ = (age_pos_ + 1) % kAgeWindow;
    if (age_n_ < kAgeWindow) ++age_n_;
    have_seq_ = true;
    last_seq_ = s.seq;
    newest_t_ = s.game_time;
    Samp q{};
    q.t = s.game_time;
    for (int i = 0; i < 3; ++i) {
        q.k.p[i] = s.pos[i];
        q.k.v[i] = s.vel[i];
        q.k.a[i] = replay_math::wrap_pi(s.ang[i]);
        q.k.w[i] = s.rates[i];
    }
    if (pn_ == kPending) drain_one();
    qpend_[pn_] = q_from_euler(q.k.a);
    pending_[pn_++] = q;
    ++st_.rx;
    return true;
}

void RemoteJet::restart() {
    n_ = pn_ = 0;
    have_seq_ = false;
    for (int i = 0; i < 3; ++i) acc_[i] = omega_[i] = 0.0;
    iv_n_ = iv_pos_ = age_n_ = age_pos_ = 0;
    ++st_.restarts;
}

RemoteJet::Out RemoteJet::shown(double now, Kin* out) {
    if (!finite(now)) return Out::None;
    double elapsed = have_last_ ? now - last_step_ : 0.0;
    elapsed = elapsed > 0.0 ? (elapsed < p_.max_step ? elapsed : p_.max_step) : 0.0;
    if (elapsed > 0.0) step_len_ = elapsed;
    have_last_ = true;
    last_step_ = now;
    touch(now);
    if (p_.mode == JetParams::Mode::Interpolate && iv_n_ > 0 && age_n_ > 0) {
        const double target = replay_math::clampd(
            percentile(age_, age_n_, 0.9) + p_.delay_interval_factor * median(iv_, iv_n_), p_.delay_min, p_.delay_max);
        delay_ = ramp_delay(delay_, target, elapsed);
    }

    // Move the shown state on to now along the path as it was before any new State.
    bool resync = false, reclass = false;
    if (have_shown_) {
        if (now < track_t_ - 1e-9) {
            // The receiver's clock stepped back. Assume one step's real time passed (keep moving), and move our own
            // times with the clock.
            const double shift = (track_t_ - now) + step_len_;
            track_t_ -= shift;
            off_t_ -= shift;
            large_until_ -= shift;
            ht_[0] -= shift;
            ht_[1] -= shift;
            track_to(now);
            ++st_.resyncs;
            reclass = true;
        } else if (!coasting_ && now - track_t_ > kMaxTrackGap) {
            resync = true;
            ++st_.resyncs;
        } else if (!coasting_) {
            track_to(now);
        }
    }
    Quat q_before = q_last_;
    double p_old[3] = {ps_[0], ps_[1], ps_[2]};
    if (have_shown_ && n_ > 0) {
        Kin k;
        double ff[3];
        Quat qr;
        raw_at(now, &k, ff, &qr);
        shown_attitude(now, qr, &q_before);
        std::memcpy(p_old, k.p, sizeof(p_old));
    }
    const bool fresh = pn_ > 0, first = !have_shown_;
    bool resume = false;
    if (fresh) {
        resume = have_shown_ && (coasting_ || (n_ > 0 && pending_[0].t - ring_[n_ - 1].t > p_.gap_snap));
        while (pn_ > 0) drain_one();
        coasting_ = false;
    }
    if (n_ == 0) return Out::None;
    if (now - ring_[n_ - 1].t > p_.coast_after) {
        if (!coasting_) {
            coasting_ = true;
            hist_n_ = 0;
        }
        ++st_.coasting_steps;
        return Out::Coast;
    }

    Kin raw;
    double ff[3];
    Quat qr;
    raw_at(now, &raw, ff, &qr);
    bool snap = first || resume || resync, snap_ang = false;
    if (!snap && (fresh || reclass)) {
        // The offset is what is shown minus the new raw path. The position class is set by how far the new State
        // moved the raw path, so a manoeuvre the prediction is still catching up with stays a small correction.
        const double d = dist3(ps_, raw.p), jump = reclass ? d : dist3(p_old, raw.p);
        double r[3];
        q_log(q_mul(q_before, q_conj(qr)), r);
        const double da = norm3(r);
        // Position and attitude snap separately: a rotation past snap_ang after a gap need not kink the path.
        if (d > p_.snap_pos || jump > p_.snap_pos) {
            snap = true;
        } else {
            if (jump >= p_.small_pos) large_until_ = now + p_.blend_long;
            st_.offset_max = std::fmax(st_.offset_max, d);
        }
        if (da > p_.snap_ang) {
            snap_ang = true;
        } else if (!snap) {
            std::memcpy(off_r_, r, sizeof(off_r_));
            off_t_ = now;
            off_dur_a_ = p_.blend_ang;
        }
    }
    if (snap || snap_ang) {
        std::memset(off_r_, 0, sizeof(off_r_));
        off_dur_a_ = 0;
        if (resync) ++st_.resync_snaps;  // a re-anchor after a long step gap: written at once, but not new data
        else ++st_.snaps;
    }
    if (snap) {
        std::memcpy(ps_, raw.p, sizeof(ps_));
        std::memcpy(vs_, raw.v, sizeof(vs_));
        large_until_ = now;
        track_t_ = now;
        hist_n_ = 0;
    }
    const bool large = now < large_until_;
    spring_w_ = kSettle / (large ? p_.blend_long : p_.blend_short);
    spring_a_ = large ? p_.blend_accel_large : p_.blend_accel;

    Quat qs;
    const bool ang = shown_attitude(now, qr, &qs);
    q_last_ = qs;
    have_shown_ = true;
    *out = raw;
    std::memcpy(out->p, ps_, sizeof(ps_));
    std::memcpy(out->v, vs_, sizeof(vs_));
    q_to_euler(qs, out->a);
    const double lag = dist3(ps_, raw.p) + dist3(vs_, raw.v);
    st_.horizon_max = std::fmax(st_.horizon_max, render_time(now) - ring_[n_ - 1].t);
    track_accel(now, out->p);
    if (snap || snap_ang) return Out::Snap;
    return (ang || lag > 1e-3) ? Out::Blend : Out::Steady;
}

JetStats RemoteJet::take_stats() {
    JetStats s = st_;
    const double span = last_now_ - win_start_;
    s.rate_hz = span > 0 ? s.rx / span : 0.0;
    s.age_p50 = age_n_ ? percentile(age_, age_n_, 0.5) : 0.0;
    s.age_p90 = age_n_ ? percentile(age_, age_n_, 0.9) : 0.0;
    s.delay = p_.mode == JetParams::Mode::Interpolate ? delay_ : 0.0;
    st_ = JetStats{};
    win_start_ = last_now_;
    return s;
}

double RemoteJet::percentile(const double* v, int n, double q) {
    if (n <= 0) return 0.0;
    if (n > kAgeWindow) n = kAgeWindow;
    double t[kAgeWindow];
    for (int i = 0; i < n; ++i) {
        int j = i;
        while (j > 0 && t[j - 1] > v[i]) {
            t[j] = t[j - 1];
            --j;
        }
        t[j] = v[i];
    }
    const int k = static_cast<int>(std::ceil(q * n)) - 1;
    return t[k < 0 ? 0 : (k >= n ? n - 1 : k)];
}

void RemoteJet::drain_one() {
    if (n_ == kRing) {
        std::memmove(&ring_[0], &ring_[1], sizeof(Samp) * (kRing - 1));
        std::memmove(&qring_[0], &qring_[1], sizeof(Quat) * (kRing - 1));
        --n_;
    }
    ring_[n_] = pending_[0];
    qring_[n_++] = qpend_[0];
    std::memmove(&pending_[0], &pending_[1], sizeof(Samp) * (kPending - 1));
    std::memmove(&qpend_[0], &qpend_[1], sizeof(Quat) * (kPending - 1));
    --pn_;
    fit();
}

void RemoteJet::fit() {
    const Samp& b = ring_[n_ - 1];
    int j = n_ - 1 - p_.accel_samples;
    if (j < 0) j = 0;
    while (j < n_ - 1 && b.t - ring_[j].t > p_.accel_span) ++j;
    const double h = b.t - ring_[j].t;
    for (int i = 0; i < 3; ++i) acc_[i] = h > 1e-6 ? (b.k.v[i] - ring_[j].k.v[i]) / h : 0.0;
    // Over a longer spacing a fast roll can turn 180 deg or more and alias the wrong way: keep the last valid rate.
    const double hr = n_ >= 2 ? b.t - ring_[n_ - 2].t : 0.0;
    if (hr > 1e-6 && hr <= p_.rate_span) {
        q_log(q_mul(qring_[n_ - 1], q_conj(qring_[n_ - 2])), omega_);
        for (double& w : omega_) w /= hr;
    }
}

void RemoteJet::raw_at(double now, Kin* out, double ff[3], Quat* q) const {
    const double t = render_time(now);
    const Samp& s = ring_[n_ - 1];
    if (p_.mode == JetParams::Mode::Interpolate && n_ >= 2 && t <= s.t) {
        replay_math::sample_at(ring_, n_, t, out);
        int i = n_ - 2;
        while (i > 0 && ring_[i].t > t) --i;
        const double h = ring_[i + 1].t - ring_[i].t;
        const bool inside = t >= ring_[0].t && h > 1e-6;
        for (int k = 0; k < 3; ++k) ff[k] = inside ? (ring_[i + 1].k.v[k] - ring_[i].k.v[k]) / h : 0.0;
        *q = inside ? q_slerp(qring_[i], qring_[i + 1], (t - ring_[i].t) / h) : qring_[0];
        q_to_euler(*q, out->a);
        return;
    }
    const double h = t - s.t;
    const double hc = replay_math::clampd(h, -p_.second_order, p_.second_order);
    // The attitude is turned by the rate for the horizon less rot_lag (smoothly: h - d (1 - e^(-h/d))). Between States
    // it turns at (1 - e^(-h/d)) of the rate: nearly the full rate at the 150 ms design latency, less at LAN ages
    // (pulled forward at each State). A roll that has just stopped overshoots by about rate x (age - rot_lag).
    const double d = p_.rot_lag, ha = std::fabs(hc);
    const double hr = (hc < 0 ? -1.0 : 1.0) * (d > 0 ? ha - d * (1.0 - std::exp(-ha / d)) : ha);
    for (int i = 0; i < 3; ++i) {
        out->p[i] = s.k.p[i] + s.k.v[i] * h + acc_[i] * (hc * h - 0.5 * hc * hc);
        out->v[i] = s.k.v[i] + acc_[i] * hc;
        out->w[i] = s.k.w[i];
        ff[i] = (h >= -p_.second_order && h <= p_.second_order) ? acc_[i] : 0.0;
    }
    const double r[3] = {omega_[0] * hr, omega_[1] * hr, omega_[2] * hr};
    *q = q_mul(q_exp(r), qring_[n_ - 1]);
    q_to_euler(*q, out->a);
}

void RemoteJet::track_to(double now) {
    const double span = now - track_t_;
    if (!(span > 0.0)) return;
    if (n_ == 0) {  // between a source restart and its first State: hold the velocity
        for (int i = 0; i < 3; ++i) ps_[i] += vs_[i] * span;
        track_t_ = now;
        return;
    }
    const int n = static_cast<int>(std::ceil(span / kSubStep));
    const double dt = span / n;
    // e = raw - shown. The wanted closing speed is c * |e|, but never more than can be stopped at half the
    // acceleration cap (sqrt(A |e|)), so a capped correction does not overshoot. kv = 4c makes the linear part
    // critically damped (a double pole at -2c).
    const double c = 0.5 * spring_w_, kv = 4.0 * c, A = spring_a_;
    for (int s = 0; s < n; ++s) {
        Kin k;
        double ff[3], e[3], ac[3], m2 = 0;
        Quat q;
        raw_at(track_t_ + s * dt, &k, ff, &q);
        for (int i = 0; i < 3; ++i) e[i] = k.p[i] - ps_[i];
        const double r = norm3(e);
        const double close = r > 1e-12 ? std::fmin(c * r, std::sqrt(A * r)) / r : 0.0;
        for (int i = 0; i < 3; ++i) {
            ac[i] = kv * ((k.v[i] - vs_[i]) + close * e[i]);
            m2 += ac[i] * ac[i];
        }
        const double sc = m2 > A * A ? A / std::sqrt(m2) : 1.0;
        for (int i = 0; i < 3; ++i) {
            const double a = ff[i] + ac[i] * sc;
            ps_[i] += vs_[i] * dt + 0.5 * a * dt * dt;
            vs_[i] += a * dt;
        }
    }
    track_t_ = now;
}

bool RemoteJet::shown_attitude(double now, const Quat& q_raw, Quat* q) const {
    const double tau = now - off_t_;
    if (!(off_dur_a_ > 0.0) || !(tau < off_dur_a_)) {
        *q = q_raw;
        return false;
    }
    const double w = replay_math::blend_weight(tau, off_dur_a_);
    const double r[3] = {off_r_[0] * w, off_r_[1] * w, off_r_[2] * w};
    *q = q_mul(q_exp(r), q_raw);
    return true;
}

void RemoteJet::track_accel(double now, const double p[3]) {
    if (hist_n_ == 2 && !(now - ht_[1] > 1e-4)) return;  // a repeated step time: keep the history
    if (hist_n_ == 2 && ht_[1] - ht_[0] > 1e-4) {
        const double h1 = ht_[1] - ht_[0], h2 = now - ht_[1];
        double a2 = 0;
        for (int i = 0; i < 3; ++i) {
            const double a = 2.0 * ((p[i] - hp_[1][i]) / h2 - (hp_[1][i] - hp_[0][i]) / h1) / (h1 + h2);
            a2 += a * a;
        }
        st_.accel_g_max = std::fmax(st_.accel_g_max, std::sqrt(a2) / kG);
    }
    if (hist_n_ == 2) {
        std::memcpy(hp_[0], hp_[1], sizeof(hp_[0]));
        ht_[0] = ht_[1];
        hist_n_ = 1;
    }
    std::memcpy(hp_[hist_n_], p, sizeof(hp_[0]));
    ht_[hist_n_++] = now;
}

}  // namespace LazyWorks::SF2Patch::net
