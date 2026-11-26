#pragma once
/// <summary>
/// The client's mission fetch (T5): its own ENet connection on its own thread, made in menus before the flight,
/// separate from ClientCore's in-flight session. It connects to the host, sends a MissionRequest, reassembles the
/// offer's chunks, checks the CRC and stops. A host with no mission yet is asked again every 3 s; a lost connection
/// is retried every 5 s, until stop(). No game or Windows-API dependencies beyond ENet.
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
    enum class State { Idle, Running, Done, Stopped };

    MissionFetch() = default;
    ~MissionFetch() { stop(); }
    MissionFetch(const MissionFetch&) = delete;
    MissionFetch& operator=(const MissionFetch&) = delete;

    /// <summary>Starts the thread and returns at once. host is a name or numeric IPv4 address.</summary>
    bool start(const char* host, uint16_t port, LogFn log);
    /// <summary>Ends the thread (joins it). Safe to call twice.</summary>
    void stop();
    State state() const { return state_.load(std::memory_order_acquire); }
    /// <summary>Once Done: the packed mission and its offer. True once; later calls return false.</summary>
    bool take(std::vector<uint8_t>* packed, MissionOfferMsg* offer);

private:
    void run();

    std::thread thread_;
    std::string host_;
    uint16_t port_ = 0;
    LogFn log_ = nullptr;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stop_{false};
    std::mutex mx_;
    bool taken_ = false;
    std::vector<uint8_t> packed_;  // under mx_
    MissionOfferMsg offer_{};      // under mx_
};

}  // namespace LazyWorks::SF2Patch::net
