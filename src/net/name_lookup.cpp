#include "net/name_lookup.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <new>
#include <thread>

namespace LazyWorks::SF2Patch::net {
namespace {

// Shared by the caller and the lookup thread with a reference count; the lookup thread touches only this block.
struct Lookup {
    std::atomic<int> refs{2};
    std::atomic<int> result{0};  // 0 pending, 1 resolved (ip set), 2 failed
    uint32_t ip = 0;
    char name[256] = {};
    ResolveFn fn = nullptr;
};

void release(Lookup* l) {
    if (l->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete l;
}

bool enet_resolve(const char* name, uint32_t* ip) {
    ENetAddress a{};
    if (enet_address_set_host(&a, name) != 0) return false;
    *ip = a.host;
    return true;
}

void run_lookup(Lookup* l) {
    const bool wsa = enet_initialize() == 0;  // its own Winsock reference: the caller may deinitialize first
    uint32_t ip = 0;
    const bool ok = wsa && (l->fn ? l->fn : enet_resolve)(l->name, &ip);
    if (wsa) enet_deinitialize();
    l->ip = ip;
    l->result.store(ok ? 1 : 2, std::memory_order_release);
    release(l);
}

}  // namespace

LookupResult lookup_name(const char* name, uint32_t* ip, const std::atomic<bool>& stop, int timeout_ms, ResolveFn fn) {
    Lookup* l = new (std::nothrow) Lookup;
    if (!l) return LookupResult::Failed;
    std::snprintf(l->name, sizeof(l->name), "%s", name);
    l->fn = fn;
    try {
        std::thread([l] { run_lookup(l); }).detach();
    } catch (...) {
        l->result.store(2);
        release(l);  // the thread's reference
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (l->result.load(std::memory_order_acquire) == 0 && !stop.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int result = l->result.load(std::memory_order_acquire);
    if (result == 1) *ip = l->ip;
    release(l);  // ours: an abandoned lookup frees the block when it returns
    if (result == 1) return LookupResult::Resolved;
    if (result == 2) return LookupResult::Failed;
    return stop.load() ? LookupResult::Stopped : LookupResult::TimedOut;
}

}  // namespace LazyWorks::SF2Patch::net
