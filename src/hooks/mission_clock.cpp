// The mission clock across PCs (handover task 1). The world owner (the host for now) sends its mission clocks every
// 2 s; the others set theirs through the mission's own setter (MSN slot 19) when they are more than 0.25 s off, so
// formations wake, loiter and reach timed waypoints at the owner's times. Game thread. See lazy-sf2-recon:
// network-recon §12.4.
#include <windows.h>

#include "hooks/hooks.h"
#include "hooks/world_logic.h"
#include "log.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr double kSendEvery = 2.0;  // game seconds
double g_next_send = 0;
uint32_t g_tx_epoch = 0;
uint16_t g_seq = 0;
uint64_t g_fixes = 0;
bool g_logged_first = false;

}  // namespace

void mission_clock_tick() {
    if (!host_world_active()) return;
    const double now = sim_game_time();
    if (g_tx_epoch != sim_player_epoch()) {
        g_tx_epoch = sim_player_epoch();
        g_next_send = 0;
    }
    if (now < g_next_send) return;
    g_next_send = now + kSendEvery;
    float t10 = 0, t14 = 0;
    if (!world_mission_clock(&t10, &t14)) return;
    net::MissionClockMsg m{};
    m.seq = ++g_seq;
    m.owner_ms = static_cast<uint32_t>(now * 1000.0);
    m.clock_ms = net::clock_to_wire(t10);
    m.clock2_ms = net::clock_to_wire(t14);
    uint8_t buf[sizeof(m)];
    const size_t n = net::encode(m, buf, sizeof(buf));
    if (n) host_world_send_all(buf, n, false);
}

void mission_clock_apply(const uint8_t* data, size_t len, double host_now) {
    net::MissionClockMsg m;
    if (net::decode(data, len, m) != net::Status::Ok) return;
    float t10 = 0, t14 = 0;
    if (!world_mission_clock(&t10, &t14)) return;
    const net::ClockFix f = net::clock_fix(t10, m, host_now);
    if (!f.set) {
        if (!g_logged_first) {
            g_logged_first = true;
            LOG("clock: the mission clock is within %.2f s of the owner's (%.1f s)", net::kClockTolerance, f.target);
        }
        return;
    }
    const int r = world_set_mission_clock(static_cast<float>(f.target), static_cast<float>(f.target2));
    g_logged_first = true;
    if (log_milestone(++g_fixes) || r != 0)
        LOG("clock: mission clock %.2f s -> %.2f s, the owner's (correction %llu): %s", t10, f.target,
            static_cast<unsigned long long>(g_fixes),
            r == 0 ? "set" : r == 1 ? "NOT set (the setter is not the one we read)" : "FAULTED");
}

}  // namespace LazyWorks::SF2Patch::hooks
