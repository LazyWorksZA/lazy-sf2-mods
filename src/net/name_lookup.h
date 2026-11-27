#pragma once
// A host name lookup that never makes its caller wait for the resolver longer than it chooses.
#include <atomic>
#include <cstdint>

namespace LazyWorks::SF2Patch::net {

/// <summary>Resolves a name to an IPv4 address (ENetAddress::host); true on success. Runs on the lookup thread.</summary>
using ResolveFn = bool (*)(const char* name, uint32_t* ip);

enum class LookupResult { Resolved, Failed, TimedOut, Stopped };

/// <summary>
/// Looks name up on a short-lived thread of its own (getaddrinfo can block for many seconds) and waits for it up to
/// timeout_ms, or until stop is set. On TimedOut or Stopped the lookup is abandoned: its thread frees its own state when
/// the resolver returns and never touches the caller's. fn replaces ENet's lookup (tests). Call with ENet initialized.
/// </summary>
LookupResult lookup_name(const char* name, uint32_t* ip, const std::atomic<bool>& stop, int timeout_ms,
                         ResolveFn fn = nullptr);

}  // namespace LazyWorks::SF2Patch::net
