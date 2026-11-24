#pragma once
// The host's network core: an ENet host on its own thread, and two small queues to the game thread.
// No game or Windows-API dependencies except what ENet brings, so the loopback tests run it without the game.
//
// Threads:
//  - The network thread owns ENet: it creates the host, services it every 5 ms, and destroys it. Nothing else
//    ever calls ENet.
//  - The game thread (any single caller thread, in the tests) talks to the core only through poll(), send(),
//    disconnect(), set_welcomed() and set_host_time(). Each takes a mutex for a few copies of a few dozen
//    bytes, or is an atomic. The queues are fixed rings, allocated with the object.
//  - The in ring has a reserve for control events (Connect, Disconnect, Hello, Bye: at most 4 per connection, because
//    Hello and Bye are accepted once each). States may use only the rest of the ring, and each slot only its share of
//    that (kStateQuota), so one client's States can never fill the ring and crowd another client's join or leave out.
//  - The network thread answers Pings itself (a Pong with the host game time the game thread last published), so
//    clock sync works while the game is paused or in a menu.
//  - The network thread decodes and checks every received packet before it queues it (length, magic, version,
//    type, channel, sender, number ranges), and drops bad ones with a counted log line once a second.
//    ENet itself allocates per packet; that is its own and cannot be avoided.
//  - Nothing logs while a lock is held. The log callback is only called from the network thread.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "net/protocol.h"

namespace lazysf2::net {

inline constexpr int kMaxClients = 4;
inline constexpr uint32_t kTimeoutMs = 5000;  // no packet from a peer for this long (wall clock) ends it

// What the game thread reads from the core.
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
    uint8_t data[80] = {};  // Message: a checked Hello, Ping, State or Bye, exactly as received
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

// Parses LAZYSF2_HOST: "port" or "address:port", the address a numeric IPv4 one (default 127.0.0.1) and the port
// 1..65535. Writes the address to addr (cap bytes) and the port to *port. False for anything else.
bool parse_host_spec(const char* spec, char* addr, size_t cap, uint16_t* port);

class HostCore {
public:
    using LogFn = void (*)(const char* line);  // called from the network thread only
    enum class State { Idle, Starting, Running, Failed };

    HostCore() = default;
    ~HostCore() { stop(false); }
    HostCore(const HostCore&) = delete;
    HostCore& operator=(const HostCore&) = delete;

    // Starts the network thread and returns at once (it never waits for the thread, so it is safe to call
    // where the loader lock is held). address is a numeric IPv4 address. Port 0 picks a free port; see port().
    // The thread binds, logs "net: host on udp <addr>:<port>" (or the failure) through log, and sets state().
    bool start(const char* address, uint16_t port, LogFn log);
    // Ends the thread: sockets closed, ENet shut down. With in_loader_lock the thread is not joined (a join
    // can deadlock there): stop() waits at most 1 s for it to report that it has cleaned up, and then detaches it.
    // Residual risk: that report comes just before the thread returns, so on a real FreeLibrary the thread may still
    // be running its last instructions (in this DLL's code) when the DLL is unmapped, and a thread that did not
    // finish within 1 s is still running ENet code. Either is a crash at unload. Accepted: developer tool, and
    // the game never unloads the DLL except at process exit.
    void stop(bool in_loader_lock);

    State state() const { return state_.load(std::memory_order_acquire); }
    uint16_t port() const { return port_.load(std::memory_order_acquire); }  // the bound port, once Running

    // --- Game thread ---
    // Moves up to max queued events into out. Costs one atomic load when the queue is empty.
    size_t poll(HostEvent* out, size_t max);
    // Queues a message (at most 63 bytes) for a slot. False if the queue is full or the message is too large.
    // gen is the connection generation from the HostEvent that introduced the client: a message for a slot that
    // has been reassigned since is dropped, never delivered to the new client.
    bool send(uint8_t slot, uint32_t gen, const uint8_t* data, size_t len, uint8_t channel, bool reliable);
    // Asks the network thread to disconnect the slot's peer (gracefully, after what is already queued for it).
    // False if the out queue is full (the peer's own silence timeout then ends it).
    bool disconnect(uint8_t slot, uint32_t gen);
    // State packets from a slot are only passed on while it is welcomed. Set it before sending the Welcome.
    // Ignored if gen is no longer the slot's current connection.
    void set_welcomed(uint8_t slot, uint32_t gen, bool welcomed);
    // The host's game clock, for the range check of State times. The game thread updates it every step.
    void set_host_time(double t) {
        host_time_.store(t, std::memory_order_relaxed);
        host_time_valid_.store(true, std::memory_order_release);
    }
    // The session is over (the last client left, or the player changed): the published time is stale. Pings are
    // not answered, and States are not passed, until set_host_time is called again.
    void reset_host_time() { host_time_valid_.store(false, std::memory_order_release); }
    uint32_t rtt_ms(uint8_t slot) const;  // ENet's smoothed round trip time, 0 for a bad slot
    HostCounters counters() const;

private:
    struct OutMsg {
        enum Op : uint8_t { Send = 1, Disconnect = 2 };
        Op op = Send;
        uint8_t slot = 0, channel = 0, reliable = 0, len = 0;
        uint32_t gen = 0;
        uint8_t data[63] = {};
    };
    static constexpr size_t kInCap = 512, kOutCap = 256;
    // Entries of the in ring that only control events may use: States are refused once the ring is this close to
    // full, so a client that floods States while the game is paused or in a menu cannot hide a Disconnect.
    static constexpr size_t kControlReserve = 64;
    // At most this many States per slot in the ring at once; the quotas of all slots together fit outside the reserve.
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
    // Per slot, one word: (connection generation << 1) | welcomed. 0 = the slot is free. One word, so that
    // set_welcomed's generation check and its store are a single compare-and-swap.
    std::atomic<uint64_t> slot_state_[kMaxClients] = {};
    std::atomic<uint32_t> rtt_[kMaxClients] = {};

    std::mutex in_mx_;
    HostEvent in_[kInCap];
    size_t in_head_ = 0, in_n_ = 0;
    size_t in_states_[kMaxClients] = {};  // States per slot currently in the ring (under in_mx_)
    std::atomic<uint32_t> in_count_{0};  // mirrors in_n_ for the lock-free emptiness check

    std::mutex out_mx_;
    OutMsg out_[kOutCap];
    size_t out_head_ = 0, out_n_ = 0;

    std::atomic<uint32_t> n_received_{0}, n_format_{0}, n_range_{0}, n_state_{0}, n_queue_{0}, n_control_{0}, n_quota_[kMaxClients] = {};
};

}  // namespace lazysf2::net
