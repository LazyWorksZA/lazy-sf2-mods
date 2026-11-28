#pragma once
// The periodic "net: traffic:" log line: rates from ENet's running totals. Pure, so a test can check the text.
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace LazyWorks::SF2Patch::net {

inline constexpr uint64_t kTrafficIntervalMs = 10000;
inline constexpr size_t kTrafficMaxPeers = 32;

/// <summary>One peer's state at the report: its number (slot + 1 on the host), smoothed round trip and the bytes still
/// queued for it. ENet resets a peer's own packet counters every 10 s, so per-peer rates are not available from it.</summary>
struct TrafficPeer {
    unsigned peer = 0;
    unsigned rtt_ms = 0;
    unsigned queued = 0;
};

/// <summary>Formats "net: traffic: up N B/s, down N B/s, datagrams N/s (peer k: rtt N ms, queued N B, peer ...)". Datagrams
/// are UDP packets sent by the host object (all peers). The part in brackets is left out when there are no peers.
/// Returns the length, truncated to fit.</summary>
inline int format_traffic_line(char* out, size_t cap, unsigned up, unsigned down, unsigned datagrams,
                               const TrafficPeer* peers, size_t n) {
    int len = std::snprintf(out, cap, "net: traffic: up %u B/s, down %u B/s, datagrams %u/s", up, down, datagrams);
    if (len < 0 || static_cast<size_t>(len) >= cap) return len;
    for (size_t i = 0; i < n; ++i) {
        const int w = std::snprintf(out + len, cap - static_cast<size_t>(len), "%s%u: rtt %u ms, queued %u B",
                                    i ? ", peer " : " (peer ", peers[i].peer, peers[i].rtt_ms, peers[i].queued);
        if (w < 0 || static_cast<size_t>(len + w) >= cap) return len;
        len += w;
    }
    if (n && static_cast<size_t>(len) + 1 < cap) {
        out[len++] = ')';
        out[len] = 0;
    }
    return len;
}

/// <summary>Turns ENet's running totals (32-bit, wrapping, only ever added to) into per-second rates, one report every
/// kTrafficIntervalMs.</summary>
class TrafficMeter {
public:
    /// <summary>Call every service loop. Returns true once per interval, with the line in out.</summary>
    bool tick(uint64_t now_ms, uint32_t sent_data, uint32_t recv_data, uint32_t sent_packets, const TrafficPeer* peers,
              size_t n, char* out, size_t cap) {
        if (!started_) {
            started_ = true;
            at_ = now_ms;
            sent_data_ = sent_data, recv_data_ = recv_data, sent_packets_ = sent_packets;
            return false;
        }
        const uint64_t dt = now_ms - at_;
        if (dt < kTrafficIntervalMs) return false;
        format_traffic_line(out, cap, per_second(sent_data - sent_data_, dt), per_second(recv_data - recv_data_, dt),
                            per_second(sent_packets - sent_packets_, dt), peers, n);
        at_ = now_ms;
        sent_data_ = sent_data, recv_data_ = recv_data, sent_packets_ = sent_packets;
        return true;
    }

private:
    static unsigned per_second(uint32_t delta, uint64_t dt_ms) { return static_cast<unsigned>((delta * 1000ull + dt_ms / 2) / dt_ms); }
    bool started_ = false;
    uint64_t at_ = 0;
    uint32_t sent_data_ = 0, recv_data_ = 0, sent_packets_ = 0;
};

}  // namespace LazyWorks::SF2Patch::net
