#include "net/auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace LazyWorks::SF2Patch::net {
namespace {

constexpr char kClientLabel[] = "LZSF2-AUTH-v5";
constexpr char kHostLabel[] = "LZSF2-HOST-v5";

// HMAC-SHA256 over the concatenation of parts.
bool hmac(const uint8_t (&key)[32], const uint8_t* const* parts, const size_t* lens, int n, uint8_t (&out)[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0;
    ok = ok && BCryptCreateHash(alg, &h, nullptr, 0, const_cast<PUCHAR>(key), sizeof(key), 0) >= 0;
    for (int i = 0; ok && i < n; ++i)
        ok = BCryptHashData(h, const_cast<PUCHAR>(parts[i]), static_cast<ULONG>(lens[i]), 0) >= 0;
    ok = ok && BCryptFinishHash(h, out, sizeof(out), 0) >= 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// HMAC(key, label || a || b).
bool labelled_mac(const AuthKey& key, const char* label, size_t label_len, const uint8_t (&a)[kAuthNonce],
                  const uint8_t (&b)[kAuthNonce], uint8_t (&out)[kAuthMac]) {
    if (!key.set) return false;
    const uint8_t* parts[] = {reinterpret_cast<const uint8_t*>(label), a, b};
    const size_t lens[] = {label_len, kAuthNonce, kAuthNonce};
    return hmac(key.k, parts, lens, 3, out);
}

bool same(const uint8_t (&a)[kAuthMac], const uint8_t (&b)[kAuthMac]) {
    uint8_t diff = 0;
    for (size_t i = 0; i < kAuthMac; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace

void auth_wipe(void* p, size_t n) {
    if (p && n) SecureZeroMemory(p, n);
}

void auth_wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

bool derive_auth_key(const std::string& password_utf8, const uint8_t (&salt)[kAuthSalt], uint32_t iterations,
                     AuthKey* out) {
    *out = AuthKey{};
    if (password_utf8.empty() || iterations == 0) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0;
    ok = ok && BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(password_utf8.data())),
                                     static_cast<ULONG>(password_utf8.size()), const_cast<PUCHAR>(salt), kAuthSalt,
                                     iterations, out->k, sizeof(out->k), 0) >= 0;
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) {
        auth_wipe(*out);
        return false;
    }
    std::memcpy(out->salt, salt, kAuthSalt);
    out->iterations = iterations;
    out->set = true;
    return true;
}

bool make_host_key(const std::string& password_utf8, AuthKey* out, uint32_t iterations) {
    uint8_t salt[kAuthSalt];
    return auth_random(salt, sizeof(salt)) && derive_auth_key(password_utf8, salt, iterations, out);
}

bool auth_random(uint8_t* out, size_t n) {
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool auth_challenge(const AuthKey& key, AuthChallengeMsg* out) {
    *out = AuthChallengeMsg{};
    if (!key.set || !auth_random(out->nonce, kAuthNonce)) return false;
    std::memcpy(out->salt, key.salt, kAuthSalt);
    out->iterations = key.iterations;
    return true;
}

bool auth_answer(const AuthKey& key, const AuthChallengeMsg& challenge, AuthAnswerMsg* out) {
    *out = AuthAnswerMsg{};
    return auth_random(out->client_nonce, kAuthNonce) &&
           labelled_mac(key, kClientLabel, sizeof(kClientLabel) - 1, challenge.nonce, out->client_nonce, out->mac);
}

bool auth_verify(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const AuthAnswerMsg& answer) {
    uint8_t want[kAuthMac];
    const bool ok = labelled_mac(key, kClientLabel, sizeof(kClientLabel) - 1, host_nonce, answer.client_nonce, want) &&
                    same(want, answer.mac);
    auth_wipe(want, sizeof(want));
    return ok;
}

bool auth_host_proof(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const AuthAnswerMsg& answer,
                     AuthProofMsg* out) {
    *out = AuthProofMsg{};
    return labelled_mac(key, kHostLabel, sizeof(kHostLabel) - 1, answer.client_nonce, host_nonce, out->mac);
}

bool auth_check_host(const AuthKey& key, const AuthChallengeMsg& challenge, const AuthAnswerMsg& answer,
                     const AuthProofMsg& proof) {
    uint8_t want[kAuthMac];
    const bool ok = labelled_mac(key, kHostLabel, sizeof(kHostLabel) - 1, answer.client_nonce, challenge.nonce, want) &&
                    same(want, proof.mac);
    auth_wipe(want, sizeof(want));
    return ok;
}

bool AuthClient::key_for(const AuthChallengeMsg& challenge, AuthKey* out) {
    if (challenge.iterations < kAuthMinIterations || challenge.iterations > kAuthMaxIterations) return false;
    if (!key_.set || key_.iterations != challenge.iterations ||
        std::memcmp(key_.salt, challenge.salt, kAuthSalt) != 0) {
        if (!derive_auth_key(password_, challenge.salt, challenge.iterations, &key_)) return false;
    }
    *out = key_;
    return true;
}

AuthLimiter::Entry* AuthLimiter::find(uint32_t ip) {
    for (Entry& e : e_)
        if (e.used && e.ip == ip) return &e;
    return nullptr;
}

const AuthLimiter::Entry* AuthLimiter::find(uint32_t ip) const {
    for (const Entry& e : e_)
        if (e.used && e.ip == ip) return &e;
    return nullptr;
}

bool AuthLimiter::allow(uint32_t ip, uint64_t now_ms, bool* first_refusal) {
    *first_refusal = false;
    Entry* e = find(ip);
    if (!e || now_ms >= e->until) return true;
    *first_refusal = !e->logged;
    e->logged = true;
    return false;
}

bool AuthLimiter::failed(uint32_t ip, uint64_t now_ms) {
    Entry* e = find(ip);
    if (!e) {
        e = &e_[0];
        for (Entry& c : e_) {
            if (!c.used) {
                e = &c;
                break;
            }
            if (c.last < e->last) e = &c;
        }
        *e = Entry{};
        e->used = true;
        e->ip = ip;
    }
    ++e->failures;
    e->last = now_ms;
    e->logged = false;
    if (e->failures >= static_cast<uint32_t>(limits_.block_after)) {
        e->until = now_ms + static_cast<uint64_t>(limits_.block_ms);
        return e->failures == static_cast<uint32_t>(limits_.block_after);
    }
    const uint32_t shift = std::min<uint32_t>(e->failures - 1, 16);
    e->until = now_ms + (static_cast<uint64_t>(limits_.cooldown_ms) << shift);
    return false;
}

void AuthLimiter::succeeded(uint32_t ip) {
    if (Entry* e = find(ip)) *e = Entry{};
}

uint64_t AuthLimiter::until(uint32_t ip) const {
    const Entry* e = find(ip);
    return e ? e->until : 0;
}

}  // namespace LazyWorks::SF2Patch::net
