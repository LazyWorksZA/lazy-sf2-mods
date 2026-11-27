#pragma once
/// <summary>
/// The client's network core: an ENet client on its own thread, plus small queues to the game thread. The client-side
/// counterpart of HostCore, with the same rules: no game or Windows-API dependencies beyond what ENet brings, so the
/// loopback tests run it without the game.
/// </summary>
/// <remarks>
/// Threads: the network thread owns ENet and the ClockSync. It resolves the host name, connects, sends the Hello, waits
/// for the Welcome, then sends a Ping every 250 ms and feeds the Pongs to the ClockSync, so the clock and the
/// keep-alive run while the game thread is paused or loading. The game thread (any single caller in the tests) uses only
/// poll(), send_state(), bye(), clock_now(), clock_snapshot() and the atomics, each a short mutex or an atomic; nothing
/// allocates, logs or does I/O under those locks.
/// The in ring reserves room for control events (Welcome, PeerInfo, PeerLeft, Bye) and gives each source id a State
/// quota (kStateQuota), so a flood of States cannot hide a Bye. The end of the connection (Disconnected) is not in the
/// ring at all: poll() always delivers it, after everything else.
/// Every packet is decoded and checked before it is queued; bad ones are dropped with a counted log line once a second.
/// A State is passed only from a source the host has announced with a PeerInfo and not yet with a PeerLeft (the host
/// itself included), so the game side can create a remote jet on the first State and drop it on PeerLeft for good.
/// There is no reconnect in here: after Disconnected the thread has ended, and the game side decides what to do.
/// </remarks>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "net/clock_sync.h"
#include "net/host_core.h"
#include "net/auth.h"
#include "net/protocol.h"

namespace LazyWorks::SF2Patch::net {

/// <summary>Why a client connection ended (ClientEvent::reason of a Disconnected event).</summary>
enum class ClientEnd : uint8_t {
    HostClosed = 0,     // the host disconnected (after its Bye, or without one)
    Silent = 1,         // nothing from the host for kTimeoutMs
    CannotConnect = 2,  // the name did not resolve, ENet failed, or no connection within the connect limit
    NoWelcome = 3,      // connected, but no Welcome within the Welcome limit
    Left = 4,           // we said Bye (bye()) and disconnected
    Refused = 5,        // we refused the host: another protocol version (a Bye with reason 6 came first)
    QueueFull = 6,      // a control event could not be queued: the game thread is not draining
};

/// <summary>What the game thread reads from the core.</summary>
struct ClientEvent {
    enum Kind : uint8_t { Welcome, PeerInfo, PeerLeft, State, Bye, Disconnected };
    Kind kind = Welcome;
    uint8_t reason = 0;  // Disconnected: a ClientEnd
    uint16_t len = 0;    // bytes in data (0 for Disconnected)
    double wall_rx = 0;  // ClientCore::wall_now() when the packet arrived: pass it to clock_now() for its host time
    uint8_t data[96] = {};  // a checked Welcome, PeerInfo, PeerLeft, State or Bye, exactly as received
};

/// <summary>A world message (world_proto.h WorldStates or WorldEvents) from the host, checked, as received.</summary>
struct WorldPacket {
    double wall_rx = 0;
    uint16_t len = 0;
    uint8_t data[1000] = {};
};

struct ClientCounters {
    uint32_t received = 0;        // packets that arrived from ENet
    uint32_t dropped_format = 0;  // wrong size, magic, version, type, channel or id
    uint32_t dropped_range = 0;   // a number out of range
    uint32_t dropped_state = 0;   // in the wrong phase, repeated, or over the Pong rate
    uint32_t dropped_queue = 0;   // States refused: their source was over its quota, or the ring was nearly full
    uint32_t dropped_control = 0; // control events lost (the ring was completely full); the connection is then ended
    uint32_t dropped_out = 0;     // our States not sent: the out queue was full, or they were late (after a Bye)
    uint32_t pings = 0, pongs = 0;  // Pings sent, Pongs the ClockSync accepted
    uint32_t world = 0, dropped_world = 0;  // world messages queued; refused (ring full, or before the Welcome)
};

/// <summary>Limits a test may shorten. The defaults are the protocol's.</summary>
struct ClientLimits {
    uint32_t connect_ms = 5000;   // ENet connection
    uint32_t welcome_ms = 15000;  // the host's game thread may be loading
    uint32_t silence_ms = kTimeoutMs;
    /// <summary>Tests only: replaces ENet's name lookup (ipv4 as ENetAddress::host). Runs on the lookup thread.</summary>
    using ResolveFn = bool (*)(const char* name, uint32_t* ipv4);
    ResolveFn resolve = nullptr;
};

class ClientCore {
public:
    using LogFn = void (*)(const char* line);  // called from the network thread only
    enum class Phase { Idle, Connecting, AwaitWelcome, Welcomed, Closed, Failed };

    ClientCore() = default;
    ~ClientCore() { stop(false); }
    ClientCore(const ClientCore&) = delete;
    ClientCore& operator=(const ClientCore&) = delete;

    /// <summary>The steady wall clock every time in this class is in, in seconds.</summary>
    static double wall_now();

    /// <summary>
    /// Starts the network thread and returns at once (never waits for it, so it is safe under the loader lock).
    /// host is a numeric IPv4 address (preferred) or a name, which the network thread resolves. The Hello is sent as
    /// given. False if already started (also after the connection ended: call stop() first) or the arguments are bad;
    /// every later failure arrives as Disconnected. The lookup of a name is bounded by limits.connect_ms.
    /// </summary>
    bool start(const char* host, uint16_t port, const HelloMsg& hello, LogFn log, const AuthClient& auth,
               const ClientLimits& limits = {});
    /// <summary>
    /// Ends the thread (a Bye queued by bye() just before still gets a short chance to go out). With in_loader_lock
    /// the thread is not joined: waits at most 1 s for it to report clean-up, then detaches it (HostCore::stop has the
    /// accepted residual risk).
    /// </summary>
    void stop(bool in_loader_lock);

    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    /// <summary>The network thread has cleaned up (after Disconnected): stop() then joins at once.</summary>
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    /// <summary>The id the Welcome gave us (1..kMaxClients), 0 before it.</summary>
    uint16_t client_id() const { return id_.load(std::memory_order_acquire); }

    // --- Game thread ---
    /// <summary>Moves up to max queued events into out; Disconnected comes last, once. One atomic load when empty.</summary>
    size_t poll(ClientEvent* out, size_t max);
    /// <summary>
    /// Queues a State for the unreliable channel; its client_id is set to ours. False unless Welcomed (and no Bye
    /// yet), or if the queue is full. The caller holds States while clock_now() is false (the host is paused).
    /// </summary>
    bool send_state(const StateMsg& s);
    /// <summary>
    /// Sends a Bye (once), then disconnects when it is delivered. Before the Welcome it only disconnects; before the
    /// connection it ends at once. After a Bye from the host the client leaves by itself within about a second.
    /// </summary>
    void bye(uint8_t reason);
    /// <summary>
    /// The host's game time at wall (ClientCore::wall_now()). False while not synced (no Welcome and Pong yet), on
    /// hold (the host is paused, or no Pong for ClockSync::kStale), or after the connection ended.
    /// </summary>
    bool clock_now(double wall, double* host_time) const;
    /// <summary>A copy of the ClockSync, for a HostClock on the game thread. False before the Welcome or after the end.</summary>
    bool clock_snapshot(ClockSync* out) const;
    uint32_t rtt_ms() const { return rtt_.load(std::memory_order_relaxed); }  // ENet's smoothed round trip time
    ClientCounters counters() const;
    /// <summary>
    /// Moves up to max queued world messages into out (game thread), the events first. They arrive only once
    /// welcomed. States are dropped (counted) when their ring is full; events never are: a full events ring ends the
    /// connection (QueueFull), as a lost control event does, since the world would diverge.
    /// </summary>
    size_t poll_world(WorldPacket* out, size_t max);
    /// <summary>Queues a reliable world message (at most 96 bytes: AvatarState) for the host. Welcomed only.</summary>
    bool send_world(const uint8_t* data, size_t len, bool reliable = true);
    static constexpr size_t kWorldStatesIn = 64, kWorldEventsIn = 256;

    static constexpr size_t kInCap = 512, kOutCap = 64;
    static constexpr int kSources = kMaxClients + 1;  // State sources: the host (0) and clients 1..kMaxClients
    /// <summary>Entries of the in ring that only control events may use.</summary>
    static constexpr size_t kControlReserve = 64;
    /// <summary>At most this many States per source id in the ring at once; together they fit outside the reserve.</summary>
    static constexpr size_t kStateQuota = (kInCap - kControlReserve) / kSources;

private:
    struct OutMsg {
        uint8_t len = 0;
        uint8_t reliable = 0;
        uint8_t data[96] = {};
    };
    static_assert(sizeof(ClientEvent::data) >= kMaxMessage && sizeof(OutMsg::data) >= kMaxMessage,
                  "every protocol message must fit the queues");
    static_assert(kStateQuota * kSources <= kInCap - kControlReserve, "the State quotas must fit outside the reserve");
    // A Welcome, a Bye and a PeerInfo and PeerLeft for every other peer, several times over.
    static_assert(kControlReserve >= 2 + 2 * kSources * 4, "the control reserve is too small");

    void run();
    bool push_in(const ClientEvent& e, int source);  // source -1 for a control event
    /// <summary>Network thread: records the end of the connection, which poll() delivers last.</summary>
    void set_end(ClientEnd why, Phase phase, double wall);

    std::thread thread_;
    char host_[256] = {};
    uint16_t port_ = 0;
    HelloMsg hello_{};
    AuthClient auth_;  // the session password: answers the host's challenge and checks its proof
    ClientLimits limits_{};
    LogFn log_ = nullptr;
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<uint16_t> id_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};  // the network thread has cleaned up and is about to return
    std::atomic<int> bye_reason_{-1};    // set once by bye(); -1 none
    std::atomic<uint32_t> rtt_{0};
    bool started_ = false;  // game thread: start() ran at least once

    ClockSync clock_;  // network thread only
    mutable std::mutex clock_mx_;
    ClockSync clock_snap_;     // under clock_mx_: the copy the game thread reads
    bool clock_valid_ = false;  // under clock_mx_: welcomed and not ended

    std::mutex in_mx_;
    ClientEvent in_[kInCap];
    size_t in_head_ = 0, in_n_ = 0;
    size_t in_states_[kSources] = {};  // States per source currently in the ring (under in_mx_)
    bool end_pending_ = false, end_delivered_ = false;  // under in_mx_
    ClientEvent end_{};                                 // under in_mx_: the Disconnected event
    std::atomic<uint32_t> in_count_{0};  // in_n_ plus a pending end, for the lock-free emptiness check

    struct WorldRing {
        explicit WorldRing(size_t c) : buf(std::make_unique<WorldPacket[]>(c)), cap(c) {}
        std::unique_ptr<WorldPacket[]> buf;  // heap
        size_t cap, head = 0, n = 0;
    };
    std::mutex world_mx_;
    WorldRing world_states_{kWorldStatesIn}, world_events_{kWorldEventsIn};  // under world_mx_
    std::atomic<uint32_t> n_world_{0}, n_world_drop_{0};

    std::mutex out_mx_;
    OutMsg out_[kOutCap];
    size_t out_head_ = 0, out_n_ = 0;

    std::atomic<uint32_t> n_received_{0}, n_format_{0}, n_range_{0}, n_state_{0}, n_queue_{0}, n_control_{0}, n_pings_{0},
        n_pongs_{0}, n_out_{0};
};

}  // namespace LazyWorks::SF2Patch::net
