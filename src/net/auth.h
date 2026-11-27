#pragma once
// Session password: challenge and response over HMAC-SHA256 (Windows CNG). The password never travels; each connection
// gets a fresh host nonce, so a recorded answer does not open another connection.
#include <cstddef>
#include <cstdint>
#include <string>

#include "net/protocol.h"

namespace LazyWorks::SF2Patch::net {

/// <summary>SHA-256 of the password's UTF-8 bytes; the only form a running session keeps.</summary>
struct AuthKey {
    uint8_t k[32] = {};
    bool set = false;
};

/// <summary>The key for a password (UTF-8). False for an empty password or a hashing failure.</summary>
bool make_auth_key(const std::string& password_utf8, AuthKey* out);

/// <summary>Cryptographically random bytes (BCryptGenRandom). False on failure.</summary>
bool auth_random(uint8_t* out, size_t n);

/// <summary>HMAC-SHA256(key, "LZSF2-AUTH-v5" || host_nonce || client_nonce). False on a hashing failure.</summary>
bool auth_mac(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const uint8_t (&client_nonce)[kAuthNonce],
              uint8_t (&out)[kAuthMac]);

/// <summary>Client: the answer to a host's challenge (a fresh client nonce). False on a failure.</summary>
bool auth_answer(const AuthKey& key, const AuthChallengeMsg& challenge, AuthAnswerMsg* out);

/// <summary>Host: whether the answer proves the key for the nonce this host sent (constant-time compare).</summary>
bool auth_verify(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const AuthAnswerMsg& answer);

/// <summary>Milliseconds a host waits for the answer before it ends the connection.</summary>
inline constexpr int kAuthTimeoutMs = 10000;

}  // namespace LazyWorks::SF2Patch::net
