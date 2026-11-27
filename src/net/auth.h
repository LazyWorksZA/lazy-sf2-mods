#pragma once
// Session password: mutual challenge and response over HMAC-SHA256 (Windows CNG). The password never travels. The key is
// PBKDF2-SHA256 of the password over a salt the host draws for each session, so a captured exchange does not give the
// password cheaply. Each connection gets a fresh host nonce and client nonce: a recorded answer or proof does not fit
// another connection, and the host's proof uses another label than the client's answer, so one is never the other.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "net/protocol.h"

namespace LazyWorks::SF2Patch::net {

/// <summary>PBKDF2 iterations a host asks for (about 50-100 ms once per session and connection on a desktop PC).</summary>
inline constexpr uint32_t kAuthIterations = 100000;
/// <summary>The range a client accepts from a host: no cheaper key than a host makes, and no more than about 150 ms of
/// work (a rogue host could otherwise make each reconnect cost seconds).</summary>
inline constexpr uint32_t kAuthMinIterations = kAuthIterations, kAuthMaxIterations = 300000;

/// <summary>The key derived from the password for one salt; the only form the host keeps.</summary>
struct AuthKey {
    uint8_t k[32] = {};
    uint8_t salt[kAuthSalt] = {};
    uint32_t iterations = 0;
    bool set = false;
};

/// <summary>Wipes a secret buffer so it does not linger in memory.</summary>
void auth_wipe(void* p, size_t n);
inline void auth_wipe(AuthKey& k) {
    auth_wipe(&k, sizeof(k));
    k.set = false;
}
/// <summary>Wipes a string's characters, then empties it.</summary>
void auth_wipe(std::string& s);

/// <summary>PBKDF2-SHA256(password UTF-8, salt, iterations). False for an empty password or a failure.</summary>
bool derive_auth_key(const std::string& password_utf8, const uint8_t (&salt)[kAuthSalt], uint32_t iterations,
                     AuthKey* out);
/// <summary>Host: the session's key, over a fresh random salt and kAuthIterations (or the given count, tests).</summary>
bool make_host_key(const std::string& password_utf8, AuthKey* out, uint32_t iterations = kAuthIterations);

/// <summary>Cryptographically random bytes (BCryptGenRandom). False on failure.</summary>
bool auth_random(uint8_t* out, size_t n);

/// <summary>Host: the challenge for one connection (fresh nonce; the key's salt and iterations).</summary>
bool auth_challenge(const AuthKey& key, AuthChallengeMsg* out);
/// <summary>Client: the answer to a challenge, with the key derived for its salt (fresh client nonce).</summary>
bool auth_answer(const AuthKey& key, const AuthChallengeMsg& challenge, AuthAnswerMsg* out);
/// <summary>Host: whether the answer proves the key for the nonce this host sent (constant-time compare).</summary>
bool auth_verify(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const AuthAnswerMsg& answer);
/// <summary>Host: its proof for an accepted answer (another label, the nonces in the other order).</summary>
bool auth_host_proof(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const AuthAnswerMsg& answer,
                     AuthProofMsg* out);
/// <summary>Client: whether the host's proof checks out for this connection's nonces (constant-time compare).</summary>
bool auth_check_host(const AuthKey& key, const AuthChallengeMsg& challenge, const AuthAnswerMsg& answer,
                     const AuthProofMsg& proof);

/// <summary>
/// Client side: the password (kept for the session, wiped on destruction) and the key derived for the latest salt, so
/// a host's repeated challenges (the mission fetch reconnects every few seconds) cost one derivation per host session.
/// </summary>
class AuthClient {
public:
    AuthClient() = default;
    explicit AuthClient(std::string password) : password_(std::move(password)) {}
    AuthClient(const AuthClient& o) : password_(o.password_), key_(o.key_) {}
    AuthClient& operator=(const AuthClient& o) {
        if (this != &o) {
            auth_wipe(password_);
            password_ = o.password_;
            key_ = o.key_;
        }
        return *this;
    }
    ~AuthClient() { clear(); }
    void clear() {
        auth_wipe(password_);
        auth_wipe(key_);
    }
    bool has_password() const { return !password_.empty(); }
    /// <summary>The key for this challenge's salt and iterations (derived once per salt). False if the iterations are
    /// out of range or the derivation fails.</summary>
    bool key_for(const AuthChallengeMsg& challenge, AuthKey* out);

private:
    std::string password_;
    AuthKey key_{};
};

/// <summary>Host: unanswered connections one address may hold at a time.</summary>
inline constexpr int kMaxPendingPerAddress = 2;
/// <summary>Unanswered connections from one IPv4 /24 at most, so one network cannot fill every slot. Floods from many
/// networks at once are an accepted risk (they can keep players waiting, not get in).</summary>
inline constexpr int kMaxPendingPerNet = 2;

/// <summary>Host: limits on connections that have not proven the password yet.</summary>
struct AuthLimits {
    int deadline_ms = 3000;  // an answer must come this soon after the challenge
    int grace_ms = 300;      // a refused connection is cut this long after its Bye (time for the Bye to arrive)
    int cooldown_ms = 1000;  // after a failure the address may try again after this, doubled per failure in a row
    int block_after = 5;     // failures in a row before the address is blocked
    int block_ms = 600000;   // how long a block lasts
    int evict_after_ms = 1500;  // an unanswered connection may make room for a newcomer only this long after its
                                // challenge (key derivation plus round trip), or once refused
    int evict_gap_ms = 2000;    // one address makes room this often at most
    int evict_table = 16;       // addresses remembered for evict_gap_ms (at most 64); when all are recent, nobody
                                // makes room, so rotating addresses cannot get round the gap
};

/// <summary>
/// Host: failed password answers per source address (IPv4), for exponential backoff and blocks. Takes the time as an
/// argument, so it is unit tested. At most kSlots addresses are remembered; the least recently seen is replaced.
/// </summary>
class AuthLimiter {
public:
    explicit AuthLimiter(const AuthLimits& limits = {}) : limits_(limits) {}
    /// <summary>Whether a connection from ip may be challenged now. *first_refusal: the first refusal of the current
    /// cooldown or block (log it once).</summary>
    bool allow(uint32_t ip, uint64_t now_ms, bool* first_refusal);
    /// <summary>A wrong or late answer from ip: the cooldown doubles; block_after in a row blocks it. True if this
    /// failure started a block.</summary>
    bool failed(uint32_t ip, uint64_t now_ms);
    /// <summary>A correct answer from ip: its count starts again.</summary>
    void succeeded(uint32_t ip);
    /// <summary>Until when ip waits (0: free).</summary>
    uint64_t until(uint32_t ip) const;
    const AuthLimits& limits() const { return limits_; }

private:
    struct Entry {
        uint32_t ip = 0;
        uint32_t failures = 0;
        uint64_t until = 0;
        uint64_t last = 0;
        bool logged = false;
        bool used = false;
    };
    static constexpr int kSlots = 64;
    Entry* find(uint32_t ip);
    const Entry* find(uint32_t ip) const;
    AuthLimits limits_;
    Entry e_[kSlots] = {};
};

}  // namespace LazyWorks::SF2Patch::net
