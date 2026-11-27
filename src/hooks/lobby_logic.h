#pragma once
// Lobby decisions with no game calls (unit tested): the address field and the callsign sent on the wire.
#include <cstdint>
#include <cstdio>
#include <string>

#include "net/client_logic.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The UDP port the lobby hosts on, and joins when the address names none.</summary>
inline constexpr uint16_t kLobbyPort = 7777;

/// <summary>
/// The Address field as host and port: "host" or "host:port", the host a numeric IPv4 address or a name, the port
/// kLobbyPort when missing. False for anything else (spaces, non-ASCII, a bad port).
/// </summary>
inline bool parse_lobby_address(const std::wstring& text, char* host, size_t cap, uint16_t* port) {
    std::string a;
    for (wchar_t c : text) {
        if (c <= 0x20 || c > 0x7e) return false;
        a += static_cast<char>(c);
    }
    if (a.empty() || a.size() > 70) return false;
    const size_t colon = a.find(':');
    if (colon == std::string::npos) a += ":" + std::to_string(kLobbyPort);
    return net::client_logic::parse_connect_spec(a.c_str(), host, cap, port);
}

/// <summary>The callsign for the wire: printable ASCII only (others dropped), at most 15 characters, else fallback.</summary>
inline std::string lobby_callsign(const std::wstring& text, const char* fallback) {
    std::string out;
    for (wchar_t c : text) {
        if (out.size() == 15) break;
        if (c >= 0x20 && c <= 0x7e) out += static_cast<char>(c);
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return fallback;
    out.erase(0, first);
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}

}  // namespace LazyWorks::SF2Patch::hooks
