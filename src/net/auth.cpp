#include "net/auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>

namespace LazyWorks::SF2Patch::net {
namespace {

constexpr char kLabel[] = "LZSF2-AUTH-v5";

// One SHA-256 (hmac_key == nullptr) or HMAC-SHA256 over the concatenation of parts.
bool hash(const uint8_t* hmac_key, size_t key_len, const uint8_t* const* parts, const size_t* lens, int n,
          uint8_t (&out)[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                          hmac_key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) >= 0;
    ok = ok && BCryptCreateHash(alg, &h, nullptr, 0, const_cast<PUCHAR>(hmac_key), static_cast<ULONG>(key_len), 0) >= 0;
    for (int i = 0; ok && i < n; ++i)
        ok = BCryptHashData(h, const_cast<PUCHAR>(parts[i]), static_cast<ULONG>(lens[i]), 0) >= 0;
    ok = ok && BCryptFinishHash(h, out, sizeof(out), 0) >= 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace

bool make_auth_key(const std::string& password_utf8, AuthKey* out) {
    *out = AuthKey{};
    if (password_utf8.empty()) return false;
    const uint8_t* parts[] = {reinterpret_cast<const uint8_t*>(password_utf8.data())};
    const size_t lens[] = {password_utf8.size()};
    out->set = hash(nullptr, 0, parts, lens, 1, out->k);
    return out->set;
}

bool auth_random(uint8_t* out, size_t n) {
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool auth_mac(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const uint8_t (&client_nonce)[kAuthNonce],
              uint8_t (&out)[kAuthMac]) {
    if (!key.set) return false;
    const uint8_t* parts[] = {reinterpret_cast<const uint8_t*>(kLabel), host_nonce, client_nonce};
    const size_t lens[] = {sizeof(kLabel) - 1, kAuthNonce, kAuthNonce};
    return hash(key.k, sizeof(key.k), parts, lens, 3, out);
}

bool auth_answer(const AuthKey& key, const AuthChallengeMsg& challenge, AuthAnswerMsg* out) {
    *out = AuthAnswerMsg{};
    return auth_random(out->client_nonce, kAuthNonce) && auth_mac(key, challenge.nonce, out->client_nonce, out->mac);
}

bool auth_verify(const AuthKey& key, const uint8_t (&host_nonce)[kAuthNonce], const AuthAnswerMsg& answer) {
    uint8_t want[kAuthMac];
    if (!auth_mac(key, host_nonce, answer.client_nonce, want)) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < kAuthMac; ++i) diff |= static_cast<uint8_t>(want[i] ^ answer.mac[i]);
    SecureZeroMemory(want, sizeof(want));
    return diff == 0;
}

}  // namespace LazyWorks::SF2Patch::net
