#include "net/host_session.h"

#include <cmath>
#include <cstring>

namespace LazyWorks::SF2Patch::net {

void join_position(const double host_pos[3], double heading, int slot, double out[3]) {
    const double sh = std::sin(heading), ch = std::cos(heading);
    const double right = kJoinRight * (slot + 1), ahead = -kJoinBack;
    out[0] = host_pos[0] + ahead * sh + right * ch;
    out[1] = host_pos[1] + ahead * ch - right * sh;
    out[2] = host_pos[2];
}

bool SendTicker::due(double now) {
    if (!started_ || now < next_ - period_ || now - next_ > period_) {
        started_ = true;
        next_ = now + period_;
        return true;
    }
    if (now < next_) return false;
    next_ += period_;
    return true;
}

namespace {
// Receivers compare aircraft types: an unknown one is said as such, never left empty.
const char* known(const char* s) { return s && s[0] ? s : "unknown"; }
}  // namespace

void HostPeers::set_host(const char* callsign, const char* aircraft) {
    host_.id = 0;
    set_str(host_.callsign, known(callsign));
    set_str(host_.aircraft, known(aircraft));
}

template <class M>
void HostPeers::send(uint8_t slot, uint32_t gen, const M& m, bool reliable) {
    uint8_t buf[kMaxMessage];
    const size_t n = encode(m, buf, sizeof(buf));
    if (!n || !core_.send(slot, gen, buf, n, reliable ? kChannelReliable : kChannelUnreliable, reliable)) ++send_fail_;
}

bool HostPeers::welcome(uint8_t slot, uint32_t gen, const WelcomeMsg& w, const char* callsign, const char* aircraft) {
    if (slot >= kMaxClients) return false;
    // States may pass from now on, and only then does the client learn it may send them.
    core_.set_welcomed(slot, gen, true);
    uint8_t buf[kMaxMessage];
    const size_t n = encode(w, buf, sizeof(buf));
    if (!n || !core_.send(slot, gen, buf, n, kChannelReliable, true)) {
        core_.set_welcomed(slot, gen, false);
        return false;
    }
    send(slot, gen, host_, true);
    Peer& p = peers_[slot];
    p = Peer{};
    p.info.id = static_cast<uint16_t>(slot + 1);
    set_str(p.info.callsign, known(callsign));
    set_str(p.info.aircraft, known(aircraft));
    for (int s = 0; s < kMaxClients; ++s) {
        if (s == slot || !peers_[s].on) continue;
        send(slot, gen, peers_[s].info, true);
        send(static_cast<uint8_t>(s), peers_[s].gen, p.info, true);
    }
    p.on = true;
    p.gen = gen;
    return true;
}

void HostPeers::leave(uint8_t slot, uint32_t gen, uint8_t reason, bool announce) {
    if (slot >= kMaxClients || !peers_[slot].on || peers_[slot].gen != gen) return;
    core_.set_welcomed(slot, gen, false);
    peers_[slot] = Peer{};
    if (!announce) return;
    PeerLeftMsg m{};
    m.id = static_cast<uint16_t>(slot + 1);
    m.reason = reason;
    for (int s = 0; s < kMaxClients; ++s)
        if (peers_[s].on) send(static_cast<uint8_t>(s), peers_[s].gen, m, true);
}

int HostPeers::send_state(const uint8_t* data, size_t len, int except) {
    int n = 0;
    for (int s = 0; s < kMaxClients; ++s) {
        if (s == except || !peers_[s].on) continue;
        if (core_.send(static_cast<uint8_t>(s), peers_[s].gen, data, len, kChannelUnreliable, false)) ++n;
        else ++send_fail_;
    }
    return n;
}

void HostPeers::relay(uint8_t slot, const uint8_t* data, size_t len) {
    if (slot >= kMaxClients || !peers_[slot].on || len == 0 || len > sizeof(Peer::pending)) return;
    Peer& p = peers_[slot];
    if (p.pending_len) ++superseded_;
    std::memcpy(p.pending, data, len);
    p.pending_len = static_cast<uint8_t>(len);
}

int HostPeers::flush_relay() {
    int n = 0;
    for (int s = 0; s < kMaxClients; ++s) {
        Peer& p = peers_[s];
        if (!p.pending_len) continue;
        n += send_state(p.pending, p.pending_len, s);
        p.pending_len = 0;
        ++relayed_from_[s];
    }
    return n;
}

int HostPeers::count() const {
    int n = 0;
    for (const Peer& p : peers_) n += p.on ? 1 : 0;
    return n;
}

}  // namespace LazyWorks::SF2Patch::net
