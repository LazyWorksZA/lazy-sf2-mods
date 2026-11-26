#pragma once
/// <summary>
/// The host's side of the v2 session protocol, without the game: the join position, the 20 Hz send schedule, and the
/// peer table that sends the Welcome, the PeerInfos and PeerLefts, and fans States out over a HostCore. host.cpp uses
/// it on the game thread; the loopback tests drive it against ClientCore.
/// </summary>
#include <cstddef>
#include <cstdint>

#include "net/host_core.h"
#include "net/protocol.h"

namespace LazyWorks::SF2Patch::net {

/// <summary>How often the host sends its own jet, in host game time (plan D9: fixed).</summary>
inline constexpr double kSendHz = 20.0;
/// <summary>Join position of client 1 in the host's heading frame; each further client is another kJoinRight out.</summary>
inline constexpr double kJoinRight = 150.0, kJoinBack = 50.0;

/// <summary>
/// The join position of the client in slot (0-based): the host's position plus, in the host's heading frame,
/// kJoinRight * (slot + 1) to the right and kJoinBack back, at the host's altitude. heading is the angle from +y
/// towards +x (the game's yaw, ang[1]): ahead is (sin, cos), right is (cos, -sin).
/// </summary>
void join_position(const double host_pos[3], double heading, int slot, double out[3]);

/// <summary>True when both theatre hashes are known and differ (the client is then refused with Bye 5).</summary>
constexpr bool theatre_mismatch(uint32_t host, uint32_t client) { return host != 0 && client != 0 && host != client; }

/// <summary>
/// A fixed-rate schedule in host time. due() is true at most once per period and keeps the average rate when steps
/// do not divide the period. After a gap of more than a period (a pause, a long frame) or a step back in time it
/// starts again from now, so it never sends a burst.
/// </summary>
class SendTicker {
public:
    explicit SendTicker(double hz = kSendHz) : period_(1.0 / hz) {}
    bool due(double now);
    void reset() { started_ = false; }

private:
    double period_;
    double next_ = 0;
    bool started_ = false;
};

/// <summary>
/// Rate limit for the host's refusal lines: a client that is refused again and again (it retries every few seconds)
/// is logged at most once per kEvery seconds per key (callsign and reason), with the count of the lines not logged.
/// A small table; the oldest key gives way to a new one.
/// </summary>
class RepeatLog {
public:
    static constexpr int kSlots = 8;
    static constexpr double kEvery = 60.0;
    /// <summary>True if this refusal is to be logged; then *suppressed is how many of the same key were not.</summary>
    bool should_log(uint32_t key, double t, unsigned* suppressed) {
        *suppressed = 0;
        int oldest = 0;
        for (int i = 0; i < kSlots; ++i) {
            Slot& s = slots_[i];
            if (s.used && s.key == key) {
                if (t - s.last < kEvery) {
                    ++s.count;
                    return false;
                }
                *suppressed = s.count;
                s.count = 0;
                s.last = t;
                return true;
            }
            if (!s.used || (slots_[oldest].used && s.last < slots_[oldest].last)) oldest = i;
        }
        slots_[oldest] = Slot{true, key, t, 0};
        return true;
    }

private:
    struct Slot {
        bool used = false;
        uint32_t key = 0;
        double last = 0;
        unsigned count = 0;
    };
    Slot slots_[kSlots];
};

/// <summary>
/// The welcomed clients and what the others must hear about them. Game thread only (any single caller in the tests).
/// Every message goes through HostCore::send; refusals (the out queue is full) are counted for the caller's 1 Hz report.
/// </summary>
class HostPeers {
public:
    explicit HostPeers(HostCore& core) : core_(core) {}

    /// <summary>The host's own PeerInfo: callsign and aircraft type (ASCII, truncated to the wire fields).</summary>
    void set_host(const char* callsign, const char* aircraft);
    /// <summary>
    /// Lets the slot's States through, sends the Welcome, then a PeerInfo for the host and for each other welcomed
    /// client, and announces the new client to the others. False if the Welcome could not be queued: the slot is then
    /// not added, its States are blocked again, and nothing else was sent.
    /// </summary>
    bool welcome(uint8_t slot, uint32_t gen, const WelcomeMsg& w, const char* callsign, const char* aircraft);
    /// <summary>
    /// Removes the slot (if it holds that connection), blocks its States, drops its pending relay, and (with announce)
    /// sends a PeerLeft to the rest. The host's session end passes false: every client gets a Bye anyway.
    /// </summary>
    void leave(uint8_t slot, uint32_t gen, uint8_t reason, bool announce = true);
    /// <summary>Sends an encoded State to every welcomed slot except the one at except (-1: to all). Returns the count.</summary>
    int send_state(const uint8_t* data, size_t len, int except);
    /// <summary>
    /// Keeps a client's State for the next flush_relay(), replacing (and counting) an older one from the same slot. A
    /// receiver only wants the newest, and relaying a backlog after a host hitch would multiply it by the client count.
    /// </summary>
    void relay(uint8_t slot, const uint8_t* data, size_t len);
    /// <summary>Sends each slot's pending State to the other welcomed slots, once. Returns the sends queued.</summary>
    int flush_relay();
    /// <summary>States of the slot that flush_relay() has fanned out since the start (one per flush, whatever the recipients).</summary>
    uint32_t relayed_from(uint8_t slot) const { return slot < kMaxClients ? relayed_from_[slot] : 0; }
    /// <summary>States replaced before they were relayed, since the last call.</summary>
    unsigned take_superseded() {
        const unsigned n = superseded_;
        superseded_ = 0;
        return n;
    }

    bool welcomed(uint8_t slot) const { return slot < kMaxClients && peers_[slot].on; }
    int count() const;
    /// <summary>Messages the core refused since the last call.</summary>
    unsigned take_send_fail() {
        const unsigned n = send_fail_;
        send_fail_ = 0;
        return n;
    }

private:
    struct Peer {
        bool on = false;
        uint32_t gen = 0;
        PeerInfoMsg info{};
        uint8_t pending_len = 0;  // a State waiting for flush_relay(), 0 = none
        uint8_t pending[kMaxMessage] = {};
    };
    template <class M>
    void send(uint8_t slot, uint32_t gen, const M& m, bool reliable);

    HostCore& core_;
    Peer peers_[kMaxClients];
    PeerInfoMsg host_{};
    unsigned send_fail_ = 0;
    unsigned superseded_ = 0;
    uint32_t relayed_from_[kMaxClients] = {};
};

}  // namespace LazyWorks::SF2Patch::net
