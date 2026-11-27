#include "net/host_core.h"

#include "net/world_proto.h"
#include "net/handover_format.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "net/receiver_math.h"

namespace LazyWorks::SF2Patch::net {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

constexpr uint32_t kMaxPingsPerSecond = 100;  // per peer; more are dropped
constexpr uint32_t kMaxLobbyPerSecond = 4;    // LobbyStatus per connection (a client sends one a round, plus changes)
constexpr size_t kMaxPacket = sizeof(HostEvent::data);
constexpr size_t kMaxServiceLoop = 512;  // events handled per pass, so a flood cannot starve the queues

}  // namespace

bool parse_host_spec(const char* spec, char* addr, size_t cap, uint16_t* port) {
    if (!spec || !addr || !port || cap < 16) return false;
    const char* colon = std::strrchr(spec, ':');
    const char* port_text = colon ? colon + 1 : spec;
    char host[16] = "127.0.0.1";
    if (colon) {
        const size_t n = static_cast<size_t>(colon - spec);
        if (n < 7 || n > 15) return false;  // "1.2.3.4" .. "255.255.255.255"
        int dots = 0, run = 0;
        for (size_t i = 0; i < n; ++i) {
            const char c = spec[i];
            if (c == '.') {
                if (run == 0) return false;
                ++dots;
                run = 0;
            } else if (c >= '0' && c <= '9') {
                if (++run > 3) return false;
            } else {
                return false;  // names are not resolved: a numeric address only
            }
            host[i] = c;
        }
        if (dots != 3 || run == 0) return false;
        host[n] = 0;
    }
    if (!*port_text) return false;
    unsigned long v = 0;
    for (const char* p = port_text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + static_cast<unsigned long>(*p - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    std::snprintf(addr, cap, "%s", host);
    *port = static_cast<uint16_t>(v);
    return true;
}

bool HostCore::start(const char* address, uint16_t port, LogFn log, const AuthKey& key) {
    if (thread_.joinable() || !address || !key.set) return false;
    key_ = key;
    std::snprintf(addr_, sizeof(addr_), "%s", address);
    req_port_ = port;
    log_ = log;
    stop_.store(false);
    finished_.store(false);
    port_.store(0);
    state_.store(State::Starting, std::memory_order_release);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }
    return true;
}

void HostCore::stop(bool in_loader_lock) {
    if (!thread_.joinable()) return;
    stop_.store(true);
    if (in_loader_lock) {
        // The thread's exit needs the loader lock, which the caller holds: wait for its cleanup, not for its exit.
        for (int i = 0; i < 1000 && !finished_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        thread_.detach();
    } else {
        thread_.join();
    }
    state_.store(State::Idle, std::memory_order_release);
}

bool HostCore::push_in(const HostEvent& e, bool control) {
    bool over_quota = false;
    {
        std::lock_guard<std::mutex> lock(in_mx_);
        over_quota = !control && in_states_[e.slot] >= kStateQuota;
        if (!over_quota && in_n_ < (control ? kInCap : kInCap - kControlReserve)) {
            in_[(in_head_ + in_n_) % kInCap] = e;
            ++in_n_;
            if (!control) ++in_states_[e.slot];
            in_count_.store(static_cast<uint32_t>(in_n_), std::memory_order_release);
            return true;
        }
    }
    (control ? n_control_ : n_queue_).fetch_add(1, std::memory_order_relaxed);
    if (over_quota) n_quota_[e.slot].fetch_add(1, std::memory_order_relaxed);
    return false;
}

size_t HostCore::poll(HostEvent* out, size_t max) {
    if (in_count_.load(std::memory_order_acquire) == 0) return 0;
    std::lock_guard<std::mutex> lock(in_mx_);
    size_t n = 0;
    while (n < max && in_n_ > 0) {
        if (in_[in_head_].is_state && in_states_[in_[in_head_].slot] > 0) --in_states_[in_[in_head_].slot];
        out[n++] = in_[in_head_];
        in_head_ = (in_head_ + 1) % kInCap;
        --in_n_;
    }
    in_count_.store(static_cast<uint32_t>(in_n_), std::memory_order_release);
    return n;
}

bool HostCore::send(uint8_t slot, uint32_t gen, const uint8_t* data, size_t len, uint8_t channel, bool reliable) {
    if (slot >= kMaxClients || !data || len == 0 || len > sizeof(OutMsg::data) || channel >= kChannelCount) return false;
    std::lock_guard<std::mutex> lock(out_mx_);
    if (out_n_ >= (reliable ? kOutCap : kOutCap - kOutReserve)) return false;
    OutMsg& m = out_[(out_head_ + out_n_) % kOutCap];
    m.op = OutMsg::Send;
    m.slot = slot;
    m.gen = gen;
    m.channel = channel;
    m.reliable = reliable ? 1 : 0;
    m.len = static_cast<uint8_t>(len);
    std::memcpy(m.data, data, len);
    ++out_n_;
    return true;
}

bool HostCore::disconnect(uint8_t slot, uint32_t gen) {
    if (slot >= kMaxClients) return false;
    std::lock_guard<std::mutex> lock(out_mx_);
    if (out_n_ >= kOutCap) return false;
    OutMsg& m = out_[(out_head_ + out_n_) % kOutCap];
    m.op = OutMsg::Disconnect;
    m.slot = slot;
    m.gen = gen;
    m.len = 0;
    ++out_n_;
    return true;
}

void HostCore::set_welcomed(uint8_t slot, uint32_t gen, bool welcomed) {
    if (slot >= kMaxClients || gen == 0) return;
    uint64_t cur = slot_state_[slot].load(std::memory_order_acquire);
    const uint64_t want = (static_cast<uint64_t>(gen) << 1) | (welcomed ? 1u : 0u);
    // Only while the slot still belongs to this generation; the check and the store are one atomic step.
    while (static_cast<uint32_t>(cur >> 1) == gen && cur != 0) {
        if (slot_state_[slot].compare_exchange_weak(cur, want, std::memory_order_acq_rel)) return;
    }
}

uint32_t HostCore::rtt_ms(uint8_t slot) const {
    return slot < kMaxClients ? rtt_[slot].load(std::memory_order_relaxed) : 0;
}

HostCounters HostCore::counters() const {
    HostCounters c;
    c.received = n_received_.load(std::memory_order_relaxed);
    c.dropped_format = n_format_.load(std::memory_order_relaxed);
    c.dropped_range = n_range_.load(std::memory_order_relaxed);
    c.dropped_state = n_state_.load(std::memory_order_relaxed);
    c.dropped_queue = n_queue_.load(std::memory_order_relaxed);
    c.dropped_control = n_control_.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxClients; ++i) c.dropped_quota[i] = n_quota_[i].load(std::memory_order_relaxed);
    return c;
}

bool HostCore::send_world(uint8_t slot, uint32_t gen, const uint8_t* data, size_t len, bool reliable) {
    if (slot >= kMaxClients || !data || len == 0 || len > sizeof(WorldOut::data)) return false;
    std::lock_guard<std::mutex> lock(world_mx_);
    WorldRing& r = reliable ? world_events_ : world_states_;
    if (r.n >= r.cap) return false;
    WorldOut& m = r.buf[(r.head + r.n) % r.cap];
    m.slot = slot;
    m.gen = gen;
    m.reliable = reliable ? 1 : 0;
    m.len = static_cast<uint16_t>(len);
    std::memcpy(m.data, data, len);
    ++r.n;
    return true;
}

int HostCore::seat_for(uint32_t tag) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    return lobby_ ? book_.seat_of(tag) : seats_.claim(tag);
}

void HostCore::release_seat(uint32_t tag) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    if (!lobby_) seats_.release(tag);  // in the lobby a seat stays with its pilot until it leaves or expires
}

void HostCore::enable_lobby(uint32_t content_hash) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    lobby_ = true;
    content_hash_ = content_hash;
}

uint8_t HostCore::lobby_host(const char* callsign, int want_seat) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    return book_.set_host(callsign, want_seat);
}

void HostCore::set_lobby_info(const char* const lines[3]) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    for (int i = 0; i < 3; ++i) set_str(summary_[i], lines[i]);
}

void HostCore::set_lobby_launch(uint32_t launch_seq) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    launch_seq_ = launch_seq;
}

HostCore::LobbyView HostCore::lobby_view() const {
    std::lock_guard<std::mutex> lock(mission_mx_);
    LobbyView v;
    v.members = book_.members();
    v.clients = book_.clients();
    v.host_seat = book_.host_seat();
    v.mission_id = mission_ ? mission_->id : 0;
    v.launch_seq = launch_seq_;
    v.count = book_.rows(v.rows, LobbyBook::kHostTag);
    std::memcpy(v.summary, summary_, sizeof(v.summary));
    return v;
}

bool HostCore::lobby_ready(uint32_t tag) const {
    std::lock_guard<std::mutex> lock(mission_mx_);
    return book_.ready(tag);
}

size_t HostCore::lobby_answer(const LobbyStatusMsg& s, uint64_t from, uint8_t* out, size_t cap) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    LobbyRosterMsg r{};
    if (s.content_hash != content_hash_) {
        r.result = kLobbyContent;
        book_.update(s.tag, s.callsign, false, -1, true, now_ms(), from);  // not in the lobby
    } else {
        // After Launch the seats are fixed: the flying pilots' Welcomes are checked against them.
        const bool flying = (launch_seq_ & kLaunchFlying) != 0;
        const int want = s.want_seat == kLobbyNoWish || flying ? -1 : s.want_seat;
        const bool ready = s.ready != 0 && mission_ && s.ready_mission == mission_->id;  // Ready for this mission only
        r.result = book_.update(s.tag, s.callsign, ready, want, s.leaving != 0, now_ms(), from);
        if (r.result == LobbyBook::kRefused) return 0;
        if (flying && s.want_seat != kLobbyNoWish && r.result == kLobbyOk && book_.seat_of(s.tag) != s.want_seat)
            r.result = kLobbySeatTaken;
    }
    r.mission_id = mission_ ? mission_->id : 0;
    r.launch_seq = launch_seq_;
    r.members = static_cast<uint8_t>(book_.members());
    const int seat = book_.seat_of(s.tag);
    r.your_seat = seat >= 0 ? static_cast<uint8_t>(seat) : kNoSeat;
    std::memcpy(r.summary, summary_, sizeof(r.summary));
    r.count = static_cast<uint8_t>(book_.rows(r.rows, s.tag));
    return encode(r, out, cap);
}

void HostCore::lobby_describe(uint32_t tag, char* out, size_t cap) const {
    std::lock_guard<std::mutex> lock(mission_mx_);
    int seat = -1;
    uint64_t seen = 0;
    if (!lobby_)
        std::snprintf(out, cap, "not a lobby");
    else if (!book_.info(tag, &seat, &seen))
        std::snprintf(out, cap, "no lobby row for tag 0x%08x (%d seats)", static_cast<unsigned>(tag), book_.members());
    else
        std::snprintf(out, cap, "lobby row of tag 0x%08x: seat %d of %d, heard of %llu ms ago", static_cast<unsigned>(tag),
                      seat, book_.members(), static_cast<unsigned long long>(now_ms() - seen));
}

int HostCore::lobby_host_seat_for(int members) {
    std::lock_guard<std::mutex> lock(mission_mx_);
    return lobby_ ? book_.host_seat_for(members) : -1;
}

bool HostCore::seat_active(uint32_t tag) const {
    std::lock_guard<std::mutex> lock(mission_mx_);
    return seats_.active(tag);
}

SeatPlan HostCore::seat_plan() const {
    std::lock_guard<std::mutex> lock(mission_mx_);
    if (lobby_) return SeatPlan{book_.members(), book_.host_seat() < 0 ? 0 : book_.host_seat()};
    return seats_.plan();
}

uint32_t HostCore::mission_id() const {
    std::lock_guard<std::mutex> lock(mission_mx_);
    return mission_ ? mission_->id : 0;
}

void HostCore::set_mission(std::vector<uint8_t> packed, uint32_t id, uint32_t raw, const SeatPlan& seats) {
    std::shared_ptr<const MissionBlob> b;
    if (!packed.empty()) {
        auto nb = std::make_shared<MissionBlob>();
        nb->packed = std::move(packed);
        nb->id = id;
        nb->raw = raw;
        b = std::move(nb);
    }
    std::lock_guard<std::mutex> lock(mission_mx_);
    mission_ = std::move(b);
    seats_.reset(seats);  // a new mission: seats are given out again
    if (lobby_) book_.set_members(seats.members, seats.host_member);  // the pilots keep their seats where the new flight has them
}

// ENet peers beyond the slots: an incoming connection can arrive while every slot is taken, so the oldest unanswered
// one can make room for it (see the Connect event).
constexpr size_t kSparePeers = 2;

// The network thread. Everything below runs on it, and only it touches ENet.
void HostCore::run() {
    char line[320];
    auto say = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof(line), fmt, args...);
        if (log_) log_(line);  // never under a lock
    };

    if (enet_initialize() != 0) {
        say("net: host failed: enet_initialize");
        state_.store(State::Failed, std::memory_order_release);
        finished_.store(true);
        return;
    }
    ENetAddress addr{};
    ENetHost* host = nullptr;
    if (enet_address_set_host_ip(&addr, addr_) == 0) {
        addr.port = req_port_;
        host = enet_host_create(&addr, kMaxClients + kSparePeers, kChannelCount, 0, 0);
    }
    if (!host) {
        say("net: host failed: cannot bind udp %s:%u (bad address, or the port is in use)", addr_,
            static_cast<unsigned>(req_port_));
        enet_deinitialize();
        state_.store(State::Failed, std::memory_order_release);
        finished_.store(true);
        return;
    }
    host->maximumPacketSize = 1024;  // our messages are under 100 bytes: refuse big ones and fragment floods
    host->maximumWaitingData = 64 * 1024;
    port_.store(host->address.port, std::memory_order_release);
    state_.store(State::Running, std::memory_order_release);
    say("net: host on udp %s:%u", addr_, static_cast<unsigned>(host->address.port));
    if (std::strncmp(addr_, "127.", 4) != 0)
        say("net: WARNING host listening on %s - reachable from the network", addr_);

    ENetPeer* peers[kMaxClients] = {};
    bool hello_seen[kMaxClients] = {};
    bool bye_seen[kMaxClients] = {};
    bool version_refused[kMaxClients] = {};
    uint64_t last_rx[kMaxClients] = {};
    uint32_t pings[kMaxClients] = {};
    uint32_t lobby_msgs[kMaxClients] = {};
    uint32_t gens[kMaxClients] = {};
    uint64_t mission_ask_ms[kMaxClients] = {};
    uint32_t slot_tag[kMaxClients] = {};  // the world tag each connection carried (request or Hello)
    // The session password: a connection is nothing to the game (no Connect event, no message, no mission) until its
    // AuthAnswer proves the key for the nonce sent to it.
    bool authed[kMaxClients] = {};
    bool auth_refused[kMaxClients] = {};
    uint8_t nonce[kMaxClients][kAuthNonce] = {};
    uint64_t auth_deadline[kMaxClients] = {};
    uint64_t refused_at[kMaxClients] = {};  // a refused connection is cut after a short grace for its Bye
    uint64_t challenged_at[kMaxClients] = {};
    struct Evictor {
        uint32_t ip = 0;
        uint64_t at = 0;
    } evictors[16] = {};  // the /24s that made room lately (least recently used entry recycled)
    AuthLimiter limiter(auth_limits_);
    auto ip_text = [](const ENetPeer* p, char* out, size_t n) {
        const uint32_t a = p->address.host;  // network byte order
        std::snprintf(out, n, "%u.%u.%u.%u", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
        return out;
    };
    uint32_t next_gen = 1;
    uint64_t next_report = now_ms() + 1000;
    HostCounters reported{};

    auto free_slot = [&](int slot) {
        peers[slot] = nullptr;
        gens[slot] = 0;
        slot_state_[slot].store(0, std::memory_order_release);  // commands for this connection are void from now on
        hello_seen[slot] = false;
        bye_seen[slot] = false;
        version_refused[slot] = false;
        authed[slot] = false;
        auth_refused[slot] = false;
        auth_deadline[slot] = 0;
        refused_at[slot] = 0;
        challenged_at[slot] = 0;
        mission_ask_ms[slot] = 0;
        // The connection's seat is released when no other open connection carries the same tag.
        if (const uint32_t tag = slot_tag[slot]) {
            slot_tag[slot] = 0;
            bool other = false;
            for (int i = 0; i < kMaxClients; ++i) other = other || (i != slot && slot_tag[i] == tag);
            if (!other) release_seat(tag);
        }
        rtt_[slot].store(0, std::memory_order_relaxed);
    };
    auto drop = [&](std::atomic<uint32_t>& counter) { counter.fetch_add(1, std::memory_order_relaxed); };

    auto send_raw = [&](int slot, const uint8_t* data, size_t n) {
        if (!peers[slot] || !n) return false;
        ENetPacket* p = enet_packet_create(data, n, ENET_PACKET_FLAG_RELIABLE);
        if (!p) return false;
        if (enet_peer_send(peers[slot], kChannelReliable, p) != 0) {
            enet_packet_destroy(p);
            return false;
        }
        return true;
    };
    // A MissionRequest is answered here, in any phase (before a Hello too, and while the game is in menus): the offer
    // and every chunk are handed to ENet at once, which paces them on the reliable channel.
    auto handle_world = [&](int slot, uint8_t channel, uint8_t wt, const uint8_t* data, size_t len) {
        if (wt == static_cast<uint8_t>(WorldType::AvatarState)) {  // a client's jet state: to the game thread
            AvatarStateMsg am;
            if (channel != kChannelReliable || decode(data, len, am) != Status::Ok) return drop(n_format_);
            if (!(slot_state_[slot].load(std::memory_order_acquire) & 1)) return drop(n_state_);  // welcomed only
            am.peer = static_cast<uint16_t>(slot + 1);  // the connection says who sent it
            HostEvent e;
            e.kind = HostEvent::Message;
            e.slot = static_cast<uint8_t>(slot);
            e.gen = gens[slot];
            e.channel = channel;
            e.len = static_cast<uint16_t>(encode(am, e.data, sizeof(e.data)));
            if (!push_in(e, true)) drop(n_queue_);
            return;
        }
        if (wt == static_cast<uint8_t>(WorldType::Loadout) || wt == static_cast<uint8_t>(WorldType::Consumables) ||
            wt == static_cast<uint8_t>(WorldType::DamageState) || wt == static_cast<uint8_t>(WorldType::Countermeasure)) {
            LoadoutMsg lm;
            ConsumablesMsg cm;
            DamageStateMsg dm;
            CountermeasureMsg km;
            const bool lo = wt != static_cast<uint8_t>(WorldType::Consumables);  // the reliable ones
            const bool ok =
                wt == static_cast<uint8_t>(WorldType::Loadout)       ? channel == kChannelReliable && decode(data, len, lm) == Status::Ok
                : wt == static_cast<uint8_t>(WorldType::Consumables) ? channel == kChannelUnreliable && decode(data, len, cm) == Status::Ok
                : wt == static_cast<uint8_t>(WorldType::DamageState) ? channel == kChannelReliable && decode(data, len, dm) == Status::Ok
                                                                      : channel == kChannelReliable && decode(data, len, km) == Status::Ok;
            if (!ok) return drop(n_format_);
            if (!(slot_state_[slot].load(std::memory_order_acquire) & 1)) return drop(n_state_);
            HostEvent e;
            e.kind = HostEvent::Message;
            e.slot = static_cast<uint8_t>(slot);
            e.gen = gens[slot];
            e.channel = channel;
            const uint16_t peer = static_cast<uint16_t>(slot + 1);
            std::memcpy(e.data, data, len);
            std::memcpy(e.data + sizeof(Header), &peer, 2);  // the connection says who sent it
            e.len = static_cast<uint16_t>(len);
            e.is_state = lo ? 0 : 1;  // consumables count against the slot's quota, as States do
            if (!push_in(e, lo)) drop(n_queue_);
            return;
        }
        if (wt == static_cast<uint8_t>(WorldType::HandoverBegin) || wt == static_cast<uint8_t>(WorldType::HandoverChunk)) {
            HandoverBeginMsg hb;
            HandoverChunkMsg hc;
            const bool ok = channel == kChannelReliable &&
                            (wt == static_cast<uint8_t>(WorldType::HandoverBegin) ? decode(data, len, hb) == Status::Ok
                                                                                  : decode(data, len, hc) == Status::Ok);
            if (!ok) return drop(n_format_);
            if (!(slot_state_[slot].load(std::memory_order_acquire) & 1)) return drop(n_state_);
            HostEvent e;
            e.kind = HostEvent::Message;
            e.slot = static_cast<uint8_t>(slot);
            e.gen = gens[slot];
            e.channel = channel;
            std::memcpy(e.data, data, len);
            e.len = static_cast<uint16_t>(len);
            if (!push_in(e, true)) drop(n_queue_);
            return;
        }
        if (wt == static_cast<uint8_t>(WorldType::WeaponLaunch)) {  // a client's weapon release: to the game thread
            WeaponLaunchMsg wm;
            if (channel != kChannelReliable || decode(data, len, wm) != Status::Ok) return drop(n_format_);
            if (!(slot_state_[slot].load(std::memory_order_acquire) & 1)) return drop(n_state_);
            wm.peer = static_cast<uint16_t>(slot + 1);
            HostEvent e;
            e.kind = HostEvent::Message;
            e.slot = static_cast<uint8_t>(slot);
            e.gen = gens[slot];
            e.channel = channel;
            e.len = static_cast<uint16_t>(encode(wm, e.data, sizeof(e.data)));
            if (!push_in(e, true)) drop(n_queue_);
            return;
        }
        if (wt == static_cast<uint8_t>(WorldType::HitClaim)) {  // a client's gun hit: to the game thread
            HitMsg hm;
            if (channel != kChannelReliable || decode_hit(data, len, WorldType::HitClaim, hm) != Status::Ok)
                return drop(n_format_);
            if (!(slot_state_[slot].load(std::memory_order_acquire) & 1)) return drop(n_state_);
            hm.peer = static_cast<uint16_t>(slot + 1);
            HostEvent e;
            e.kind = HostEvent::Message;
            e.slot = static_cast<uint8_t>(slot);
            e.gen = gens[slot];
            e.channel = channel;
            e.len = static_cast<uint16_t>(encode_hit(hm, WorldType::HitClaim, e.data, sizeof(e.data)));
            if (!push_in(e, true)) drop(n_queue_);
            return;
        }
        if (wt == static_cast<uint8_t>(WorldType::LobbyStatus)) {  // answered here, like a mission request
            LobbyStatusMsg ls;
            if (channel != kChannelReliable || decode(data, len, ls) != Status::Ok || ls.tag == 0) return drop(n_format_);
            if (!lobby_) return drop(n_state_);
            if (++lobby_msgs[slot] > kMaxLobbyPerSecond) return drop(n_state_);
            if (slot_tag[slot] && ls.tag != slot_tag[slot]) return drop(n_state_);  // one tag per connection
            slot_tag[slot] = ls.tag;
            // The fetch connects anew each round from one address, so a row is bound to that address (lobby_book.h).
            const uint64_t from = (static_cast<uint64_t>(peers[slot]->address.host) << 16) | peers[slot]->address.port;
            uint8_t out[sizeof(LobbyRosterMsg)];
            const size_t n = lobby_answer(ls, from, out, sizeof(out));
            if (!n) return drop(n_state_);  // another address's row: unanswered
            send_raw(slot, out, n);
            return;
        }
        MissionRequestMsg rq;
        if (wt != static_cast<uint8_t>(WorldType::MissionRequest) || channel != kChannelReliable ||
            decode(data, len, rq) != Status::Ok)
            return drop(n_format_);
        if (slot_tag[slot] && rq.tag != slot_tag[slot]) return drop(n_state_);  // one tag per connection
        const uint64_t ask_now = now_ms();
        if (mission_ask_ms[slot] && ask_now - mission_ask_ms[slot] < 1000) return drop(n_state_);  // 1 a second at most
        mission_ask_ms[slot] = ask_now;        std::shared_ptr<const MissionBlob> b;
        {
            std::lock_guard<std::mutex> lock(mission_mx_);
            b = mission_;
        }
        MissionOfferMsg o{};
        o.chunk_bytes = static_cast<uint16_t>(kMissionChunkData);
        o.seat = kNoSeat;
        o.host_seat = kNoSeat;
        if (!b) {
            o.status = kOfferNone;
        } else if (b->packed.size() > kMissionMaxPacked) {
            o.status = kOfferTooBig;
        } else {
            o.status = kOfferOk;
            o.mission_id = b->id;
            o.packed_bytes = static_cast<uint32_t>(b->packed.size());
            o.raw_bytes = b->raw;
            o.chunk_count = static_cast<uint16_t>(mission_chunk_count(b->packed.size()));
            // The host gives the seats, by the client's tag, never by connection slot (network-recon §10.16).
            slot_tag[slot] = rq.tag;
            const int seat = seat_for(rq.tag);
            o.seat = seat >= 0 ? static_cast<uint8_t>(seat) : kNoSeat;
            o.host_seat = static_cast<uint8_t>(seat_plan().host_member);
            if (rq.have_id == b->id) o.status = kOfferSame;  // it holds this mission: no chunks
        }
        uint8_t out[kMissionChunkMax];
        if (!send_raw(slot, out, encode(o, out, sizeof(out)))) return;
        if (o.status == kOfferSame) return;  // the regular poll of a client that is up to date: no log line
        if (o.status != kOfferOk) {
            say("net: peer %u asked for the mission: %s", static_cast<unsigned>(slot + 1),
                o.status == kOfferNone ? "none yet" : "too big to send");
            return;
        }
        size_t sent = 0;
        for (uint16_t i = 0; i < o.chunk_count; ++i)
            if (send_raw(slot, out, encode_mission_chunk(o.mission_id, i, b->packed.data(), b->packed.size(), out,
                                                         sizeof(out))))
                ++sent;
        n_missions_.fetch_add(1, std::memory_order_relaxed);
        say("net: mission 0x%08x sent to peer %u: %u bytes in %u of %u chunks (seat %u, host seat %u)",
            static_cast<unsigned>(o.mission_id), static_cast<unsigned>(slot + 1), static_cast<unsigned>(o.packed_bytes),
            static_cast<unsigned>(sent), static_cast<unsigned>(o.chunk_count), static_cast<unsigned>(o.seat),
            static_cast<unsigned>(o.host_seat));
    };

    auto send_bye = [&](int slot, uint8_t reason) {
        ByeMsg bye{};
        bye.client_id = 0;
        bye.reason = reason;
        uint8_t out[sizeof(ByeMsg)];
        send_raw(slot, out, encode(bye, out, sizeof(out)));
        enet_peer_disconnect_later(peers[slot], 0);
    };
    auto refuse_auth = [&](int slot, const char* why) {
        auth_refused[slot] = true;
        auth_deadline[slot] = 0;
        refused_at[slot] = now_ms();
        char ip[24] = "?";
        if (peers[slot]) ip_text(peers[slot], ip, sizeof(ip));
        const bool blocked = peers[slot] && limiter.failed(peers[slot]->address.host, refused_at[slot]);
        say("net: peer %u (%s) refused: %s%s", static_cast<unsigned>(slot + 1), ip, why,
            blocked ? "; that address is blocked for a while" : "");
        if (peers[slot]) send_bye(slot, kByeWrongPassword);
    };
    // Before the answer only the answer counts (and a version refusal, so an older build hears why).
    auto handle_auth = [&](int slot, uint8_t channel, const uint8_t* data, size_t len) {
        MsgType type{};
        const Status ps = peek_type(data, len, type);
        if (ps == Status::BadVersion && channel == kChannelReliable && !version_refused[slot] && peers[slot]) {
            version_refused[slot] = true;
            say("net: client %u speaks another protocol version (this host is v%u); refused", static_cast<unsigned>(slot + 1),
                static_cast<unsigned>(kVersion));
            send_bye(slot, kByeWrongVersion);
            return;
        }
        if (auth_refused[slot]) return drop(n_state_);
        AuthAnswerMsg a;
        if (ps != Status::Ok || type != MsgType::AuthAnswer || channel != kChannelReliable ||
            decode(data, len, a) != Status::Ok)
            return drop(n_state_);  // anything else before the answer is dropped unread
        if (!auth_verify(key_, nonce[slot], a)) return refuse_auth(slot, "wrong session password");
        // The host proves the password too, before anything else reaches the client.
        AuthProofMsg proof;
        uint8_t pb[sizeof(AuthProofMsg)];
        if (!auth_host_proof(key_, nonce[slot], a, &proof) || !send_raw(slot, pb, encode(proof, pb, sizeof(pb))))
            return refuse_auth(slot, "could not send the host's proof");
        limiter.succeeded(peers[slot]->address.host);
        authed[slot] = true;
        auth_deadline[slot] = 0;
        // No log line: the client's mission fetch connects every few seconds.
        HostEvent e;
        e.kind = HostEvent::Connect;
        e.slot = static_cast<uint8_t>(slot);
        e.gen = gens[slot];
        push_in(e, true);
    };

    auto handle_packet = [&](int slot, uint8_t channel, const uint8_t* data, size_t len) {
        n_received_.fetch_add(1, std::memory_order_relaxed);
        if (len > kMaxPacket) return drop(n_format_);
        if (!authed[slot]) return handle_auth(slot, channel, data, len);
        if (const uint8_t wt = peek_world_type(data, len)) return handle_world(slot, channel, wt, data, len);
        MsgType type{};
        const Status ps = peek_type(data, len, type);
        if (ps == Status::BadVersion && channel == kChannelReliable && !version_refused[slot] && peers[slot]) {
            // A peer of another protocol version (a v1 build): say why once, send a Bye it can read if it is
            // v2-framed, and end the connection. Nothing from it reaches the game thread.
            version_refused[slot] = true;
            say("net: client %u speaks another protocol version (this host is v%u); refused", static_cast<unsigned>(slot + 1),
                static_cast<unsigned>(kVersion));
            ByeMsg bye{};
            bye.client_id = 0;
            bye.reason = kByeWrongVersion;
            uint8_t out[sizeof(ByeMsg)];
            const size_t n = encode(bye, out, sizeof(out));
            if (n) {
                if (ENetPacket* p = enet_packet_create(out, n, ENET_PACKET_FLAG_RELIABLE)) {
                    if (enet_peer_send(peers[slot], kChannelReliable, p) != 0) enet_packet_destroy(p);
                }
            }
            enet_peer_disconnect_later(peers[slot], 0);
        }
        if (ps != Status::Ok) return drop(n_format_);
        const uint16_t id = static_cast<uint16_t>(slot + 1);
        switch (type) {
            case MsgType::Hello: {
                HelloMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok) return drop(n_format_);
                if (hello_seen[slot]) return drop(n_state_);
                hello_seen[slot] = true;
                slot_tag[slot] = m.world_tag;  // the session holds its seat until it closes
                break;
            }
            case MsgType::Ping: {
                PingMsg m;
                if (channel != kChannelUnreliable || decode(data, len, m) != Status::Ok) return drop(n_format_);
                if (!finite(m.client_send_time)) return drop(n_range_);
                if (++pings[slot] > kMaxPingsPerSecond) return drop(n_state_);
                // Answered here, with the host game time the game thread last published (it does not advance
                // while the game is paused, which is the truth for a clock the clients stamp States in).
                PongMsg pong{};
                pong.client_send_time = m.client_send_time;
                if (!host_time_valid_.load(std::memory_order_acquire)) return drop(n_state_);  // no time to give yet
                pong.host_game_time = host_time_.load(std::memory_order_relaxed);
                uint8_t out[sizeof(PongMsg)];
                const size_t n = encode(pong, out, sizeof(out));
                if (n && peers[slot]) {
                    if (ENetPacket* p = enet_packet_create(out, n, 0)) {
                        if (enet_peer_send(peers[slot], kChannelUnreliable, p) != 0) enet_packet_destroy(p);
                    }
                }
                return;  // never queued: the game thread has nothing to do with it
            }
            case MsgType::State: {
                StateMsg m;
                if (channel != kChannelUnreliable || decode(data, len, m) != Status::Ok || m.client_id != id)
                    return drop(n_format_);
                if (!(slot_state_[slot].load(std::memory_order_acquire) & 1) ||
                    !host_time_valid_.load(std::memory_order_acquire))
                    return drop(n_state_);
                if (!state_in_range(m, host_time_.load(std::memory_order_relaxed))) return drop(n_range_);
                break;
            }
            case MsgType::Bye: {
                ByeMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.client_id != id)
                    return drop(n_format_);
                if (bye_seen[slot]) return drop(n_state_);
                bye_seen[slot] = true;
                break;
            }
            default:  // Welcome, Pong, PeerInfo, PeerLeft and AuthChallenge only travel from the host to a client;
                return drop(n_format_);  // a second AuthAnswer is dropped too
        }
        HostEvent e;
        e.kind = HostEvent::Message;
        e.slot = static_cast<uint8_t>(slot);
        e.gen = gens[slot];
        e.channel = channel;
        e.is_state = type == MsgType::State ? 1 : 0;
        e.len = static_cast<uint16_t>(len);
        std::memcpy(e.data, data, len);
        // Control events (Hello and Bye included: once per connection each) may use the reserve; States may not, and
        // each slot has a quota of them. A Hello or Bye that still cannot be queued (practically unreachable: the
        // reserve is sized for all of them) would leave a connection the game never hears about, so it is ended.
        if (!push_in(e, type != MsgType::State) && type != MsgType::State && peers[slot]) {
            say("net: client %u connection ended: %s could not be queued", static_cast<unsigned>(slot + 1),
                type == MsgType::Hello ? "Hello" : "Bye");
            enet_peer_disconnect(peers[slot], 0);
        }
    };

    auto handle_event = [&](ENetEvent& ev) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                // Before any challenge: an address waiting after wrong answers is cut at once (logged once per wait),
                // and one address has at most kMaxPendingPerAddress unanswered connections (a player's session and its
                // mission fetch). When every slot is taken, the oldest unanswered connection of another address makes
                // room (ENet keeps spare peers for this), but only once it had time to answer (or was refused), and
                // one address makes room only every evict_gap_ms: strangers holding slots cannot starve a player, and
                // a flood of newcomers cannot cut a player whose answer is on its way.
                const uint64_t t = now_ms();
                bool first = false;
                char ip[24];
                if (!limiter.allow(ev.peer->address.host, t, &first)) {
                    if (first)
                        say("net: refusing %s for %llu s after wrong session passwords", ip_text(ev.peer, ip, sizeof(ip)),
                            static_cast<unsigned long long>((limiter.until(ev.peer->address.host) - t + 999) / 1000));
                    enet_peer_disconnect_now(ev.peer, 0);
                    break;
                }
                const uint32_t from_ip = ev.peer->address.host;
                int slot = -1, oldest = -1;
                int same_ip_pending = 0, same_net_pending = 0;
                for (int i = 0; i < kMaxClients; ++i) {
                    if (!peers[i]) {
                        if (slot < 0) slot = i;
                    } else if (!authed[i]) {
                        // address.host is in network order: its low three bytes are the /24.
                        if (net24(peers[i]->address.host) == net24(from_ip)) ++same_net_pending;
                        if (peers[i]->address.host == from_ip) {
                            ++same_ip_pending;
                            continue;  // never one of the newcomer's own address
                        }
                        const bool old_enough =
                            auth_refused[i] || t >= challenged_at[i] + static_cast<uint64_t>(auth_limits_.evict_after_ms);
                        if (!old_enough) continue;
                        // Refused ones first (they are on their way out), then the one challenged longest ago.
                        const uint64_t age_key = auth_refused[i] ? 0 : challenged_at[i];
                        const uint64_t best = oldest < 0 ? ~0ull : (auth_refused[oldest] ? 0 : challenged_at[oldest]);
                        if (oldest < 0 || age_key < best) oldest = i;
                    }
                }
                if (same_ip_pending >= kMaxPendingPerAddress ||
                    (!private_v4(from_ip) && same_net_pending >= kMaxPendingPerNet)) {
                    enet_peer_disconnect_now(ev.peer, 0);
                    break;
                }
                if (slot < 0) {
                    // The newcomer's /24 entry, else the least recently used one (so a network not seen lately can
                    // always make room); a /24 makes room once per evict_gap_ms.
                    const uint32_t net = net24(from_ip);
                    Evictor* rec = &evictors[0];
                    for (Evictor& x : evictors) {
                        if (x.at && x.ip == net) {
                            rec = &x;
                            break;
                        }
                        if (x.at < rec->at) rec = &x;
                    }
                    const bool too_soon =
                        rec->at && rec->ip == net && t < rec->at + static_cast<uint64_t>(auth_limits_.evict_gap_ms);
                    if (oldest < 0 || too_soon) {  // no connection that may make room, or this network just made some
                        enet_peer_disconnect_now(ev.peer, 0);
                        break;
                    }
                    rec->ip = net;
                    rec->at = t;
                    enet_peer_disconnect_now(peers[oldest], 0);
                    peers[oldest]->data = nullptr;
                    free_slot(oldest);
                    slot = oldest;
                }
                peers[slot] = ev.peer;
                gens[slot] = next_gen++;
                if (next_gen == 0) next_gen = 1;
                slot_state_[slot].store(static_cast<uint64_t>(gens[slot]) << 1, std::memory_order_release);
                ev.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(slot + 1));
                hello_seen[slot] = false;
                bye_seen[slot] = false;
                last_rx[slot] = now_ms();
                pings[slot] = 0;
                lobby_msgs[slot] = 0;
                authed[slot] = false;
                auth_refused[slot] = false;
                // The Connect event waits for the answer (handle_auth). A fresh nonce per connection: an answer
                // recorded from another one does not fit.
                AuthChallengeMsg ch{};
                uint8_t out[sizeof(AuthChallengeMsg)];
                if (!auth_challenge(key_, &ch) || !send_raw(slot, out, encode(ch, out, sizeof(out)))) {
                    say("net: peer %u: could not send the password challenge; connection ended", static_cast<unsigned>(slot + 1));
                    enet_peer_disconnect_now(ev.peer, 0);
                    ev.peer->data = nullptr;
                    free_slot(slot);
                    break;
                }
                std::memcpy(nonce[slot], ch.nonce, kAuthNonce);
                challenged_at[slot] = now_ms();
                auth_deadline[slot] = now_ms() + static_cast<uint64_t>(auth_limits_.deadline_ms);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {  // the peer left, or ENet's own timeout fired
                const uintptr_t tag = reinterpret_cast<uintptr_t>(ev.peer->data);
                ev.peer->data = nullptr;
                if (tag >= 1 && tag <= kMaxClients && peers[tag - 1] == ev.peer) {
                    const int slot = static_cast<int>(tag) - 1;
                    HostEvent e;
                    e.kind = HostEvent::Disconnect;
                    e.slot = static_cast<uint8_t>(slot);
                    e.gen = gens[slot];
                    const bool known = authed[slot];  // the game heard of it only once it gave the password
                    free_slot(slot);
                    if (known) push_in(e, true);
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const uintptr_t tag = reinterpret_cast<uintptr_t>(ev.peer->data);
                if (tag >= 1 && tag <= kMaxClients && peers[tag - 1] == ev.peer) {
                    const int slot = static_cast<int>(tag) - 1;
                    last_rx[slot] = now_ms();
                    handle_packet(slot, ev.channelID, ev.packet->data, ev.packet->dataLength);
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            default:
                break;
        }
    };

    auto drain_out = [&] {
        OutMsg batch[32];
        for (;;) {
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lock(out_mx_);
                while (n < 32 && out_n_ > 0) {
                    batch[n++] = out_[out_head_];
                    out_head_ = (out_head_ + 1) % kOutCap;
                    --out_n_;
                }
            }
            if (n == 0) return;
            for (size_t i = 0; i < n; ++i) {
                const OutMsg& m = batch[i];
                ENetPeer* peer = peers[m.slot];
                if (!peer || m.gen != gens[m.slot]) continue;  // a command for a connection that is gone
                if (m.op == OutMsg::Disconnect) {
                    // _later: enet_peer_disconnect would discard what is still queued for the peer (a Bye just sent).
                    enet_peer_disconnect_later(peer, 0);
                } else if (ENetPacket* p =
                               enet_packet_create(m.data, m.len, m.reliable ? ENET_PACKET_FLAG_RELIABLE : 0)) {
                    if (enet_peer_send(peer, m.channel, p) != 0) enet_packet_destroy(p);
                }
            }
        }
    };

    auto drain_world = [&] {
        for (;;) {
            WorldOut m;
            {
                std::lock_guard<std::mutex> lock(world_mx_);
                WorldRing& r = world_events_.n ? world_events_ : world_states_;  // events first
                if (r.n == 0) return;
                m = r.buf[r.head];
                r.head = (r.head + 1) % r.cap;
                --r.n;
            }
            ENetPeer* peer = peers[m.slot];
            if (!peer || m.gen != gens[m.slot]) continue;
            if (ENetPacket* p = enet_packet_create(m.data, m.len, m.reliable ? ENET_PACKET_FLAG_RELIABLE : 0))
                if (enet_peer_send(peer, m.reliable ? kChannelReliable : kChannelUnreliable, p) != 0)
                    enet_packet_destroy(p);
        }
    };

    while (!stop_.load(std::memory_order_relaxed)) {
        drain_out();
        drain_world();
        ENetEvent ev;
        int r = enet_host_service(host, &ev, 5);  // waits up to 5 ms for traffic, then returns
        for (size_t guard = 0; r > 0 && guard < kMaxServiceLoop; ++guard) {
            handle_event(ev);
            r = enet_host_service(host, &ev, 0);
        }
        drain_out();
        enet_host_flush(host);

        const uint64_t now = now_ms();
        for (int i = 0; i < kMaxClients; ++i) {
            if (!peers[i]) continue;
            rtt_[i].store(peers[i]->roundTripTime, std::memory_order_relaxed);
            if (auth_deadline[i] && now >= auth_deadline[i]) refuse_auth(i, "no password answer in time");
            if (auth_refused[i] && now - refused_at[i] >= static_cast<uint64_t>(auth_limits_.grace_ms)) {
                // Cut now rather than wait for ENet's own timeout: the slot is free for the next player at once.
                enet_peer_disconnect_now(peers[i], 0);
                peers[i]->data = nullptr;
                free_slot(i);
                continue;
            }
            const bool awaiting = hello_seen[i] && !(slot_state_[i].load(std::memory_order_acquire) & 1);
            if (now - last_rx[i] >= (awaiting ? kAwaitWelcomeMs : kTimeoutMs)) {
                enet_peer_disconnect_now(peers[i], 0);
                HostEvent e;
                e.kind = HostEvent::Disconnect;
                e.slot = static_cast<uint8_t>(i);
                e.gen = gens[i];
                e.reason = 1;
                const bool known = authed[i];
                free_slot(i);
                if (known) push_in(e, true);
            }
        }
        if (now >= next_report) {
            next_report = now + 1000;
            if (lobby_) {
                std::lock_guard<std::mutex> lock(mission_mx_);
                if (const int gone = book_.expire(now))
                    say("lobby: %d pilot(s) not heard of for %llu s; their rows and seats dropped", gone,
                        static_cast<unsigned long long>(LobbyBook::kExpireMs / 1000));
            }
            for (uint32_t& p : pings) p = 0;
            for (uint32_t& m : lobby_msgs) m = 0;
            const HostCounters c = counters();
            uint32_t quota_now = 0, quota_before = 0;
            for (int i = 0; i < kMaxClients; ++i) {
                quota_now += c.dropped_quota[i];
                quota_before += reported.dropped_quota[i];
            }
            if (c.dropped_format != reported.dropped_format || c.dropped_range != reported.dropped_range ||
                c.dropped_state != reported.dropped_state || c.dropped_queue != reported.dropped_queue ||
                c.dropped_control != reported.dropped_control) {
                say("net: dropped packets in the last second: %u malformed, %u out of range, %u wrong phase or rate, "
                    "%u States refused (%u of them over a slot's quota; the game is not draining), %u control events lost (queue full)",
                    c.dropped_format - reported.dropped_format, c.dropped_range - reported.dropped_range,
                    c.dropped_state - reported.dropped_state, c.dropped_queue - reported.dropped_queue, quota_now - quota_before,
                    c.dropped_control - reported.dropped_control);
                reported = c;
            }
        }
    }

    for (int i = 0; i < kMaxClients; ++i)
        if (peers[i]) enet_peer_disconnect_now(peers[i], 0);
    enet_host_flush(host);
    enet_host_destroy(host);
    enet_deinitialize();
    say("net: host stopped");
    finished_.store(true);
}

}  // namespace LazyWorks::SF2Patch::net
