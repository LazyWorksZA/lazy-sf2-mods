#pragma once
/// <summary>
/// The host's network core: an ENet host on its own thread, plus two small queues to the game thread. No game
/// or Windows-API dependencies beyond what ENet brings, so the loopback tests run it without the game.
/// </summary>
/// <remarks>
/// Threads: the network thread owns ENet (creates, services every 5 ms, destroys it); nothing else calls ENet.
/// The game thread (any single caller in the tests) uses only poll(), send(), disconnect(), set_welcomed() and
/// set_host_time(), each a short mutex or an atomic. The queues are fixed rings.
/// The in ring reserves room for control events (Connect, Disconnect, Hello, Bye: at most 4 per connection) and
/// gives each slot a State quota (kStateQuota), so one client's States cannot crowd out another's join or leave.
/// The network thread answers Pings itself (so clock sync works while the game is paused) and decodes and checks
/// every packet before queueing it, dropping bad ones with a counted log line once a second.
/// Nothing logs while a lock is held; the log callback is called from the network thread only.
/// </remarks>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "net/protocol.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

inline constexpr int kMaxClients = 4;
inline constexpr uint32_t kTimeoutMs = 5000;  // no packet from a peer for this long (wall clock) ends it

/// <summary>What the game thread reads from the core.</summary>
struct HostEvent {
    enum Kind : uint8_t { Connect = 1, Disconnect = 2, Message = 3 };
    Kind kind = Connect;
    uint8_t slot = 0;     // 0..kMaxClients-1; the client_id on the wire is slot + 1
    uint32_t gen = 0;     // the connection that holds the slot: bumped on every connect, never 0. Commands for a
                          // slot carry it, and the network thread ignores one whose generation is no longer current
    uint8_t is_state = 0;  // Message: a State (counted against its slot's quota until it is polled)
    uint8_t reason = 0;   // Disconnect: 0 the peer left (or ENet's own timeout), 1 no packets for kTimeoutMs
    uint8_t channel = 0;  // Message
    uint16_t len = 0;     // Message: bytes in data
    uint8_t data[96] = {};  // Message: a checked Hello, Ping, State or Bye, exactly as received
};

struct HostCounters {
    uint32_t received = 0;        // packets that arrived from ENet
    uint32_t dropped_format = 0;  // wrong size, magic, version, type or channel, or the wrong sender
    uint32_t dropped_range = 0;   // a State with a number out of range
    uint32_t dropped_state = 0;   // a State, or a repeated Hello, in the wrong phase
    uint32_t dropped_queue = 0;   // States refused: a slot was over its quota, or the ring was nearly full
    uint32_t dropped_quota[kMaxClients] = {};  // ... of which refused because that slot was over its quota
    uint32_t dropped_control = 0; // Connect or Disconnect events lost: the ring was completely full
};

/// <summary>
/// Parses LAZYSF2_HOST: "port" or "address:port", the address a numeric IPv4 one (default 127.0.0.1) and the port
/// 1..65535. Writes the address to addr (cap bytes) and the port to *port. False for anything else.
/// </summary>
bool parse_host_spec(const char* spec, char* addr, size_t cap, uint16_t* port);

class HostCore {
public:
    using LogFn = void (*)(const char* line);  // called from the network thread only
    enum class State { Idle, Starting, Running, Failed };

    HostCore() = default;
    ~HostCore() { stop(false); }
    HostCore(const HostCore&) = delete;
    HostCore& operator=(const HostCore&) = delete;

    /// <summary>
    /// Starts the network thread and returns at once (never waits for it, so it is safe under the loader lock).
    /// address is a numeric IPv4 address; port 0 picks a free port (see port()). The thread binds, logs
    /// "net: host on udp addr:port" or the failure through log, and sets state().
    /// </summary>
    bool start(const char* address, uint16_t port, LogFn log);
    /// <summary>
    /// Ends the thread. With in_loader_lock the thread is not joined (a join can deadlock there): waits at most
    /// 1 s for it to report clean-up, then detaches it. Accepted residual risk: on FreeLibrary the thread may
    /// still be running its last instructions, or ENet code, when the DLL is unmapped. Developer tool only.
    /// </summary>
    void stop(bool in_loader_lock);

    State state() const { return state_.load(std::memory_order_acquire); }
    uint16_t port() const { return port_.load(std::memory_order_acquire); }  // the bound port, once Running

    static constexpr size_t kOutCap = 256;
    /// <summary>
    /// Entries of the out ring that only reliable messages (Welcome, PeerInfo, PeerLeft, Bye) and disconnects may use:
    /// unreliable sends (States) are refused once the ring is this close to full, so a burst of States can never
    /// refuse a control message.
    /// </summary>
    static constexpr size_t kOutReserve = 64;

    // --- Game thread ---
    /// <summary>Moves up to max queued events into out. One atomic load when the queue is empty.</summary>
    size_t poll(HostEvent* out, size_t max);
    /// <summary>
    /// Queues a message (at most 96 bytes) for a slot. False if the queue is full (for an unreliable message: all but
    /// kOutReserve entries are used) or the message too large.
    /// gen is the connection generation from the HostEvent that introduced the client: a message for a slot
    /// reassigned since is dropped, never delivered to the new client.
    /// </summary>
    bool send(uint8_t slot, uint32_t gen, const uint8_t* data, size_t len, uint8_t channel, bool reliable);
    /// <summary>
    /// Asks the network thread to disconnect the slot's peer after what is already queued for it. False if the
    /// out queue is full (the peer's own silence timeout then ends it).
    /// </summary>
    bool disconnect(uint8_t slot, uint32_t gen);
    /// <summary>Lets State packets from a slot through. Set before sending the Welcome. Ignored for a stale gen.</summary>
    void set_welcomed(uint8_t slot, uint32_t gen, bool welcomed);
    /// <summary>The host's game clock, for the range check of State times. Updated every step by the game thread.</summary>
    void set_host_time(double t) {
        host_time_.store(t, std::memory_order_relaxed);
        host_time_valid_.store(true, std::memory_order_release);
    }
    /// <summary>
    /// The session is over: the published time is stale. Pings are not answered and States not passed until
    /// set_host_time is called again.
    /// </summary>
    void reset_host_time() { host_time_valid_.store(false, std::memory_order_release); }
    uint32_t rtt_ms(uint8_t slot) const;  // ENet's smoothed round trip time, 0 for a bad slot
    /// <summary>
    /// The mission package (world_proto.h pack_mission) that the network thread sends, offer and chunks, to any peer
    /// that asks with a MissionRequest, in menus as in flight. Any thread; replaces the previous one. Empty = none.
    /// </summary>
    void set_mission(std::vector<uint8_t> packed, uint32_t id, uint32_t raw, const SeatPlan& seats = {});
    /// <summary>
    /// The seat (member of the host's flight) of the client with this world tag, by the owner's rule (SeatBook):
    /// held while one of its connections is open, released when the last closes (the network thread does that), and
    /// given back to the same tag while still free. -1 for tag 0, no plan or no free member. Any thread.
    /// </summary>
    int seat_for(uint32_t tag);
    SeatPlan seat_plan() const;
    uint32_t mission_id() const;
    /// <summary>True while the tag holds a seat (one of its connections is open).</summary>
    bool seat_active(uint32_t tag) const;
    /// <summary>
    /// Queues a world message (world_proto.h, up to 1,000 bytes) for a slot. WorldStates (unreliable, channel 1) and
    /// WorldEvents (reliable, channel 0) have queues of their own, apart from the session's: the states can never
    /// fill the events' queue. False if that queue is full or the message too large; gen as for send(). A false for a
    /// reliable message means the client would diverge: the caller ends that client's session.
    /// </summary>
    bool send_world(uint8_t slot, uint32_t gen, const uint8_t* data, size_t len, bool reliable);
    static constexpr size_t kWorldStatesCap = 64, kWorldEventsCap = 256;
    /// <summary>Missions sent in full since start (offers with chunks).</summary>
    uint32_t missions_sent() const { return n_missions_.load(std::memory_order_relaxed); }
    HostCounters counters() const;

private:
    struct OutMsg {
        enum Op : uint8_t { Send = 1, Disconnect = 2 };
        Op op = Send;
        uint8_t slot = 0, channel = 0, reliable = 0, len = 0;
        uint32_t gen = 0;
        uint8_t data[96] = {};
    };
    static_assert(sizeof(HostEvent::data) >= kMaxMessage && sizeof(OutMsg::data) >= kMaxMessage,
                  "every protocol message must fit the queues");
    static constexpr size_t kInCap = 512;
    /// <summary>
    /// Entries of the in ring that only control events may use: States are refused once the ring is this close to
    /// full, so a client that floods States while the game is paused or in a menu cannot hide a Disconnect.
    /// </summary>
    static constexpr size_t kControlReserve = 64;
    /// <summary>At most this many States per slot in the ring at once; the quotas of all slots together fit outside the reserve.</summary>
    static constexpr size_t kStateQuota = (kInCap - kControlReserve) / kMaxClients;
    // The reserve covers every kind of control event for every client (4 each), twice over: enough for clients that
    // do not reconnect faster than the game drains the ring.
    static_assert(kControlReserve >= 4 * kMaxClients * 2, "the control reserve is too small");
    static_assert(kStateQuota * kMaxClients <= kInCap - kControlReserve, "the State quotas must fit outside the reserve");

    void run();
    bool push_in(const HostEvent& e, bool control);

    std::thread thread_;
    char addr_[48] = {};
    uint16_t req_port_ = 0;
    LogFn log_ = nullptr;
    std::atomic<State> state_{State::Idle};
    std::atomic<uint16_t> port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};  // the network thread has cleaned up and is about to return
    std::atomic<double> host_time_{0.0};
    std::atomic<bool> host_time_valid_{false};
    /// <summary>
    /// Per slot, one word: (connection generation &lt;&lt; 1) | welcomed. 0 = the slot is free. One word, so that
    /// set_welcomed's generation check and its store are a single compare-and-swap.
    /// </summary>
    std::atomic<uint64_t> slot_state_[kMaxClients] = {};
    std::atomic<uint32_t> rtt_[kMaxClients] = {};

    std::mutex in_mx_;
    HostEvent in_[kInCap];
    size_t in_head_ = 0, in_n_ = 0;
    size_t in_states_[kMaxClients] = {};  // States per slot currently in the ring (under in_mx_)
    std::atomic<uint32_t> in_count_{0};  // mirrors in_n_ for the lock-free emptiness check

    struct MissionBlob {
        std::vector<uint8_t> packed;
        uint32_t id = 0, raw = 0;
    };
    struct WorldOut {
        uint8_t slot = 0, reliable = 0;
        uint16_t len = 0;
        uint32_t gen = 0;
        uint8_t data[1000] = {};
    };
    struct WorldRing {
        explicit WorldRing(size_t c) : buf(std::make_unique<WorldOut[]>(c)), cap(c) {}
        std::unique_ptr<WorldOut[]> buf;  // heap
        size_t cap, head = 0, n = 0;
    };
    std::mutex world_mx_;
    WorldRing world_states_{kWorldStatesCap}, world_events_{kWorldEventsCap};  // under world_mx_
    SeatBook seats_;  // under mission_mx_
    void release_seat(uint32_t tag);
    mutable std::mutex mission_mx_;
    std::shared_ptr<const MissionBlob> mission_;  // under mission_mx_; the network thread copies the pointer
    std::atomic<uint32_t> n_missions_{0};

    std::mutex out_mx_;
    OutMsg out_[kOutCap];
    size_t out_head_ = 0, out_n_ = 0;

    std::atomic<uint32_t> n_received_{0}, n_format_{0}, n_range_{0}, n_state_{0}, n_queue_{0}, n_control_{0}, n_quota_[kMaxClients] = {};
};

}  // namespace LazyWorks::SF2Patch::net
