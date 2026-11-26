#pragma once
/// <summary>
/// The client's mission fetch (T5): its own short ENet connections on its own thread, separate from ClientCore's
/// in-flight session, so it works in menus. Every poll it connects, sends a MissionRequest with the id it holds,
/// and gets the offer: the same mission (nothing to do), none yet, or a new mission whose chunks it reassembles and
/// checks. Then it disconnects (a waiting client holds no host slot) and asks again after the poll interval, so a
/// mission the host changes is fetched in the menus before the next Accept. Runs until stop(). No game or Windows-API
/// dependencies beyond ENet.
/// </summary>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

class MissionFetch {
public:
    using LogFn = void (*)(const char* line);  // called from the fetch thread only
    enum class State { Idle, Running, Stopped };

    MissionFetch() = default;
    ~MissionFetch() { stop(); }
    MissionFetch(const MissionFetch&) = delete;
    MissionFetch& operator=(const MissionFetch&) = delete;

    /// <summary>
    /// Starts the thread and returns at once. host is a name or numeric IPv4 address; tag is the client's world tag
    /// (not 0), which the host keys its seat by; poll_ms is the time between asks.
    /// </summary>
    bool start(const char* host, uint16_t port, uint32_t tag, LogFn log, uint32_t poll_ms = 5000);
    /// <summary>Ends the thread (joins it). Safe to call twice.</summary>
    void stop();
    State state() const { return state_.load(std::memory_order_acquire); }
    /// <summary>A mission received since the last take(): true once per mission.</summary>
    bool take(std::vector<uint8_t>* packed, MissionOfferMsg* offer);
    /// <summary>The mission id the host named in its latest offer (0: none yet). Any thread.</summary>
    uint32_t latest_id() const { return latest_id_.load(std::memory_order_acquire); }
    /// <summary>The seat the host gave in its latest offer (kNoSeat: none).</summary>
    uint8_t latest_seat() const { return latest_seat_.load(std::memory_order_acquire); }

private:
    void run();

    std::thread thread_;
    std::string host_;
    uint16_t port_ = 0;
    uint32_t tag_ = 0, poll_ms_ = 5000;
    LogFn log_ = nullptr;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> latest_id_{0};
    std::atomic<uint8_t> latest_seat_{kNoSeat};
    std::mutex mx_;
    bool fresh_ = false;           // under mx_: packed_ is not taken yet
    std::vector<uint8_t> packed_;  // under mx_
    MissionOfferMsg offer_{};      // under mx_
};

}  // namespace LazyWorks::SF2Patch::net
