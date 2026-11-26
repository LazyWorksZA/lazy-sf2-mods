// RemoteJet: see remote_jet.h and the Phase 4 plan, section 2.5.
#include "net/remote_jet.h"

#include <cmath>
#include <cstring>

namespace LazyWorks::SF2Patch::net {

bool RemoteJet::push(const StateMsg& s, double now) {
    touch(now);
    const bool any = n_ + pn_ > 0;
    if ((have_seq_ && static_cast<int32_t>(s.seq - last_seq_) <= 0) || (any && !(s.game_time > newest_t_))) {
        ++st_.dup_or_old;
        return false;
    }
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
    pending_[pn_++] = q;
    ++st_.rx;
    return true;
}

RemoteJet::Out RemoteJet::shown(double now, Kin* out) {
    double elapsed = have_last_ ? now - last_step_ : 0.0;
    elapsed = elapsed > 0.0 ? (elapsed < p_.max_step ? elapsed : p_.max_step) : 0.0;
    have_last_ = true;
    last_step_ = now;
    touch(now);
    if (p_.mode == JetParams::Mode::Interpolate && iv_n_ > 0 && age_n_ > 0) {
        const double target = replay_math::clampd(
            percentile(age_, age_n_, 0.9) + p_.delay_interval_factor * median(iv_, iv_n_), p_.delay_min, p_.delay_max);
        delay_ = ramp_delay(delay_, target, elapsed);
    }

    // Move the shown state on to now along the path as it was before any new State.
    bool snap = false;
    if (n_ > 0 && !coasting_) snap = !track_to(now);
    if (pn_ > 0) {
        const bool first = n_ == 0;
        const bool resume = !first && (coasting_ || pending_[0].t - ring_[n_ - 1].t > p_.gap_snap);
        double before_a[3] = {};
        Kin old{};
        double ff[3];
        if (!first) {
            angles_at(now, before_a);
            raw_at(now, &old, ff);
        }
        while (pn_ > 0) drain_one();
        coasting_ = false;
        Kin raw;
        raw_at(now, &raw, ff);
        // The offset is what is shown minus the new raw path; its class is set by how far the new State moved
        // the raw path (a manoeuvre the prediction is catching up with stays a small, capped correction).
        double d2 = 0, j2 = 0, da = 0, dang[3] = {};
        for (int i = 0; i < 3; ++i) {
            d2 += (ps_[i] - raw.p[i]) * (ps_[i] - raw.p[i]);
            j2 += (old.p[i] - raw.p[i]) * (old.p[i] - raw.p[i]);
            dang[i] = replay_math::angle_diff(before_a[i], raw.a[i]);
            da = std::fmax(da, std::fabs(dang[i]));
        }
        const double d = std::sqrt(d2), jump = std::sqrt(j2);
        if (first || resume || snap || d > p_.snap_pos || jump > p_.snap_pos || da > p_.snap_ang) {
            snap = true;
        } else {
            if (jump >= p_.small_pos) large_until_ = now + p_.blend_long;
            const bool large = now < large_until_;
            spring_w_ = kSettle / (large ? p_.blend_long : p_.blend_short);
            spring_a_ = large ? 1e30 : p_.blend_accel;
            if (da >= 1e-9) {
                std::memcpy(off_a_, dang, sizeof(off_a_));
                off_t_ = now;
                off_dur_a_ = da < p_.small_ang ? p_.blend_short : p_.blend_long;
            }
            st_.offset_max = std::fmax(st_.offset_max, d);
        }
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
    raw_at(now, &raw, ff);
    if (snap) {
        std::memcpy(ps_, raw.p, sizeof(ps_));
        std::memcpy(vs_, raw.v, sizeof(vs_));
        std::memset(off_a_, 0, sizeof(off_a_));
        off_dur_a_ = 0;
        spring_w_ = kSettle / p_.blend_short;
        spring_a_ = p_.blend_accel;
        large_until_ = now;
        track_t_ = now;
        ++st_.snaps;
        hist_n_ = 0;
    }
    *out = raw;
    std::memcpy(out->p, ps_, sizeof(ps_));
    std::memcpy(out->v, vs_, sizeof(vs_));
    const bool ang = apply_angles(now, out->a);
    double e2 = 0;
    for (int i = 0; i < 3; ++i) e2 += (ps_[i] - raw.p[i]) * (ps_[i] - raw.p[i]) + (vs_[i] - raw.v[i]) * (vs_[i] - raw.v[i]);
    st_.horizon_max = std::fmax(st_.horizon_max, render_time(now) - ring_[n_ - 1].t);
    track_accel(now, out->p);
    if (snap) return Out::Snap;
    return (ang || e2 > 1e-4) ? Out::Blend : Out::Steady;
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
        --n_;
    }
    ring_[n_++] = pending_[0];
    std::memmove(&pending_[0], &pending_[1], sizeof(Samp) * (kPending - 1));
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
    for (int i = 0; i < 3; ++i) rate_[i] = 0.0;
    if (n_ >= 2 && b.t - ring_[n_ - 2].t <= p_.accel_span) replay_math::angle_rate(ring_[n_ - 2], b, rate_);
}

void RemoteJet::raw_at(double now, Kin* out, double ff[3]) const {
    const double t = render_time(now);
    const Samp& s = ring_[n_ - 1];
    if (p_.mode == JetParams::Mode::Interpolate && n_ >= 2 && t <= s.t) {
        replay_math::sample_at(ring_, n_, t, out);
        int i = n_ - 2;
        while (i > 0 && ring_[i].t > t) --i;
        const double h = ring_[i + 1].t - ring_[i].t;
        for (int k = 0; k < 3; ++k) {
            out->a[k] = replay_math::wrap_pi(out->a[k]);
            ff[k] = (t >= ring_[0].t && h > 1e-6) ? (ring_[i + 1].k.v[k] - ring_[i].k.v[k]) / h : 0.0;
        }
        return;
    }
    const double h = t - s.t;
    const double hc = replay_math::clampd(h, -p_.second_order, p_.second_order);
    for (int i = 0; i < 3; ++i) {
        out->p[i] = s.k.p[i] + s.k.v[i] * h + acc_[i] * (hc * h - 0.5 * hc * hc);
        out->v[i] = s.k.v[i] + acc_[i] * hc;
        out->a[i] = replay_math::wrap_pi(s.k.a[i] + rate_[i] * hc);
        out->w[i] = s.k.w[i];
        ff[i] = (h >= -p_.second_order && h <= p_.second_order) ? acc_[i] : 0.0;
    }
}

bool RemoteJet::track_to(double now) {
    const double span = now - track_t_;
    if (!(span > 0.0)) return true;
    if (span > kMaxTrackGap) return false;
    const int n = static_cast<int>(std::ceil(span / kSubStep));
    const double dt = span / n;
    // e = raw - shown. The wanted closing speed is c * |e|, but never more than can be stopped at half the
    // acceleration cap (sqrt(A |e|)), so a capped correction does not overshoot. kv = 4c makes the linear part
    // critically damped (a double pole at -2c).
    const double c = 0.5 * spring_w_, kv = 4.0 * c, A = spring_a_;
    for (int s = 0; s < n; ++s) {
        Kin k;
        double ff[3], e[3], ac[3], r2 = 0, m2 = 0;
        raw_at(track_t_ + s * dt, &k, ff);
        for (int i = 0; i < 3; ++i) {
            e[i] = k.p[i] - ps_[i];
            r2 += e[i] * e[i];
        }
        const double r = std::sqrt(r2);
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
    return true;
}

void RemoteJet::angles_at(double now, double a[3]) const {
    Kin k;
    double ff[3];
    raw_at(now, &k, ff);
    std::memcpy(a, k.a, sizeof(k.a));
    apply_angles(now, a);
}

bool RemoteJet::apply_angles(double now, double a[3]) const {
    const double tau = now - off_t_;
    if (!(off_dur_a_ > 0.0) || !(tau < off_dur_a_)) return false;
    const double w = replay_math::blend_weight(tau, off_dur_a_);
    for (int i = 0; i < 3; ++i) a[i] = replay_math::wrap_pi(a[i] + off_a_[i] * w);
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
