#include "net/world_proto.h"

#include <miniz.h>

#include <cmath>
#include <limits>
#include <cstring>

namespace LazyWorks::SF2Patch::net {

namespace {

template <class T>
size_t encode_fixed(const T& in, WorldType t, uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(T)) return 0;
    T m = in;
    m.h.magic = kMagic;
    m.h.version = kVersion;
    m.h.type = static_cast<uint8_t>(t);
    std::memcpy(out, &m, sizeof(T));
    return sizeof(T);
}

Status check(const uint8_t* data, size_t len, WorldType t) {
    if (!data || len < sizeof(Header)) return Status::Truncated;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return Status::BadMagic;
    if (h.version != kVersion) return Status::BadVersion;
    if (h.type != static_cast<uint8_t>(t)) return Status::BadType;
    return Status::Ok;
}

template <class T>
Status decode_fixed(const uint8_t* data, size_t len, WorldType t, T& out) {
    const Status s = check(data, len, t);
    if (s != Status::Ok) return s;
    if (len < sizeof(T)) return Status::Truncated;
    if (len > sizeof(T)) return Status::Oversized;
    std::memcpy(&out, data, sizeof(T));
    return Status::Ok;
}

constexpr char kPackMagic[4] = {'L', 'Z', 'W', 'M'};
constexpr uint32_t kPackVersion = 1;
constexpr size_t kPackHead = 16;

}  // namespace

uint8_t peek_world_type(const uint8_t* data, size_t len) {
    if (!data || len < sizeof(Header)) return 0;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kVersion) return 0;
    return h.type >= kWorldTypeFirst && h.type <= kWorldTypeLast ? h.type : 0;
}

size_t encode(const MissionRequestMsg& m, uint8_t* o, size_t c) {
    return encode_fixed(m, WorldType::MissionRequest, o, c);
}
size_t encode(const MissionOfferMsg& m, uint8_t* o, size_t c) { return encode_fixed(m, WorldType::MissionOffer, o, c); }
size_t encode(const AvatarStateMsg& m, uint8_t* o, size_t c) { return encode_fixed(m, WorldType::AvatarState, o, c); }
Status decode(const uint8_t* d, size_t n, AvatarStateMsg& out) { return decode_fixed(d, n, WorldType::AvatarState, out); }
Status decode(const uint8_t* d, size_t n, MissionRequestMsg& out) {
    return decode_fixed(d, n, WorldType::MissionRequest, out);
}
Status decode(const uint8_t* d, size_t n, MissionOfferMsg& out) {
    return decode_fixed(d, n, WorldType::MissionOffer, out);
}

size_t encode_mission_chunk(uint32_t mission_id, uint16_t index, const uint8_t* packed, size_t n, uint8_t* out,
                            size_t cap) {
    const size_t off = static_cast<size_t>(index) * kMissionChunkData;
    if (!out || !packed || off >= n) return 0;
    const size_t len = n - off < kMissionChunkData ? n - off : kMissionChunkData;
    if (cap < sizeof(MissionChunkHead) + len) return 0;
    MissionChunkHead h{};
    h.h.magic = kMagic;
    h.h.version = kVersion;
    h.h.type = static_cast<uint8_t>(WorldType::MissionChunk);
    h.mission_id = mission_id;
    h.index = index;
    h.len = static_cast<uint16_t>(len);
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), packed + off, len);
    return sizeof(h) + len;
}

Status decode_mission_chunk(const uint8_t* data, size_t len, MissionChunkHead& head, const uint8_t** payload) {
    const Status s = check(data, len, WorldType::MissionChunk);
    if (s != Status::Ok) return s;
    if (len < sizeof(MissionChunkHead)) return Status::Truncated;
    MissionChunkHead h;
    std::memcpy(&h, data, sizeof(h));
    if (h.len == 0 || h.len > kMissionChunkData) return Status::Oversized;
    if (len < sizeof(h) + h.len) return Status::Truncated;
    if (len > sizeof(h) + h.len) return Status::Oversized;
    head = h;
    *payload = data + sizeof(h);
    return Status::Ok;
}

bool pack_mission(const std::string& sidecar, const std::vector<uint8_t>& msn, std::vector<uint8_t>* packed,
                  uint32_t* id, uint32_t* raw) {
    const size_t total = kPackHead + sidecar.size() + msn.size();
    if (total > kMissionMaxRaw) return false;
    std::vector<uint8_t> plain(total);
    const uint32_t head[3] = {kPackVersion, static_cast<uint32_t>(sidecar.size()), static_cast<uint32_t>(msn.size())};
    std::memcpy(plain.data(), kPackMagic, 4);
    std::memcpy(plain.data() + 4, head, sizeof(head));
    if (!sidecar.empty()) std::memcpy(plain.data() + kPackHead, sidecar.data(), sidecar.size());
    if (!msn.empty()) std::memcpy(plain.data() + kPackHead + sidecar.size(), msn.data(), msn.size());
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(total));
    std::vector<uint8_t> out(cap);
    if (mz_compress2(out.data(), &cap, plain.data(), static_cast<mz_ulong>(total), MZ_BEST_COMPRESSION) != MZ_OK)
        return false;
    if (cap > kMissionMaxPacked) return false;
    out.resize(cap);
    *id = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, out.data(), out.size()));
    if (*id == 0) *id = 1;  // 0 means "none" in a MissionRequest
    *raw = static_cast<uint32_t>(total);
    *packed = std::move(out);
    return true;
}

bool unpack_mission(const std::vector<uint8_t>& packed, uint32_t id, uint32_t raw, std::string* sidecar,
                    std::vector<uint8_t>* msn, const char** why) {
    uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, packed.data(), packed.size()));
    if (crc == 0) crc = 1;
    if (crc != id) return *why = "CRC mismatch", false;
    if (raw < kPackHead || raw > kMissionMaxRaw) return *why = "bad unpacked size", false;
    std::vector<uint8_t> plain(raw);
    mz_ulong got = raw;
    if (mz_uncompress(plain.data(), &got, packed.data(), static_cast<mz_ulong>(packed.size())) != MZ_OK || got != raw)
        return *why = "does not inflate to the offered size", false;
    uint32_t head[3];
    std::memcpy(head, plain.data() + 4, sizeof(head));
    if (std::memcmp(plain.data(), kPackMagic, 4) != 0 || head[0] != kPackVersion)
        return *why = "not a mission package (magic or version)", false;
    if (static_cast<uint64_t>(head[1]) + head[2] + kPackHead != raw) return *why = "inconsistent part sizes", false;
    sidecar->assign(reinterpret_cast<const char*>(plain.data() + kPackHead), head[1]);
    msn->assign(plain.begin() + kPackHead + head[1], plain.end());
    return true;
}

namespace {
template <typename T>
T clamp_round(double v, double scale) {
    if (!std::isfinite(v)) return 0;
    const double x = std::round(v * scale);
    const double lo = static_cast<double>(std::numeric_limits<T>::min()), hi = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(x < lo ? lo : x > hi ? hi : x);
}
constexpr double kPi = 3.14159265358979323846;
constexpr double kPosScale = 100.0, kVelScale = 20.0, kRateScale = 1000.0, kAngScale = 65536.0 / (2.0 * kPi);

int16_t quantise_angle(double a) {
    if (!std::isfinite(a)) return 0;
    double t = std::fmod(a, 2.0 * kPi);
    if (t >= kPi) t -= 2.0 * kPi;
    if (t < -kPi) t += 2.0 * kPi;
    long v = std::lround(t * kAngScale);
    if (v > 32767) v -= 65536;  // pi itself wraps to -pi
    return static_cast<int16_t>(v);
}

template <class Head, class Rec>
size_t encode_batch(WorldType t, uint32_t seq, uint32_t host_ms, const Rec* r, size_t n, size_t max, uint8_t* out,
                    size_t cap) {
    if (!out || !r || n == 0 || n > max || cap < sizeof(Head) + n * sizeof(Rec)) return 0;
    Head h{};
    h.h.magic = kMagic;
    h.h.version = kVersion;
    h.h.type = static_cast<uint8_t>(t);
    h.seq = seq;
    h.host_ms = host_ms;
    h.count = static_cast<uint8_t>(n);
    std::memcpy(out, &h, sizeof(h));
    std::memcpy(out + sizeof(h), r, n * sizeof(Rec));
    return sizeof(h) + n * sizeof(Rec);
}

template <class Head, class Rec>
Status decode_batch(WorldType t, const uint8_t* data, size_t len, size_t max, Head& head, const Rec** recs) {
    const Status s = check(data, len, t);
    if (s != Status::Ok) return s;
    if (len < sizeof(Head)) return Status::Truncated;
    Head h;
    std::memcpy(&h, data, sizeof(h));
    if (h.count == 0 || h.count > max) return Status::Oversized;
    const size_t want = sizeof(Head) + h.count * sizeof(Rec);
    if (len < want) return Status::Truncated;
    if (len > want) return Status::Oversized;
    head = h;
    *recs = reinterpret_cast<const Rec*>(data + sizeof(Head));  // packed: byte alignment
    return Status::Ok;
}
}  // namespace

void quantise(const ObjPose& p, WorldObjState* q) {
    for (int i = 0; i < 3; ++i) {
        q->pos[i] = clamp_round<int32_t>(p.pos[i], kPosScale);
        q->vel[i] = clamp_round<int16_t>(p.vel[i], kVelScale);
        q->ang[i] = quantise_angle(p.ang[i]);
        q->rates[i] = clamp_round<int16_t>(p.rates[i], kRateScale);
    }
}

void dequantise(const WorldObjState& q, ObjPose* p) {
    for (int i = 0; i < 3; ++i) {
        p->pos[i] = q.pos[i] / kPosScale;
        p->vel[i] = static_cast<float>(q.vel[i] / kVelScale);
        p->ang[i] = static_cast<float>(q.ang[i] / kAngScale);
        p->rates[i] = static_cast<float>(q.rates[i] / kRateScale);
    }
}

size_t encode_world_states(uint32_t seq, uint32_t host_ms, const WorldObjState* s, size_t n, uint8_t* out, size_t cap) {
    return encode_batch<WorldStatesHead>(WorldType::WorldStates, seq, host_ms, s, n, kWorldStatesMax, out, cap);
}
Status decode_world_states(const uint8_t* data, size_t len, WorldStatesHead& head, const WorldObjState** states) {
    return decode_batch(WorldType::WorldStates, data, len, kWorldStatesMax, head, states);
}
size_t encode_world_events(uint32_t seq, uint32_t host_ms, const WorldEventRec* e, size_t n, uint8_t* out, size_t cap) {
    return encode_batch<WorldEventsHead>(WorldType::WorldEvents, seq, host_ms, e, n, kWorldEventsMax, out, cap);
}
Status decode_world_events(const uint8_t* data, size_t len, WorldEventsHead& head, const WorldEventRec** events) {
    return decode_batch(WorldType::WorldEvents, data, len, kWorldEventsMax, head, events);
}

bool MissionAssembly::begin(const MissionOfferMsg& offer) {
    *this = MissionAssembly{};
    if (offer.status != kOfferOk || offer.packed_bytes == 0 || offer.packed_bytes > kMissionMaxPacked ||
        offer.raw_bytes > kMissionMaxRaw || offer.chunk_bytes != kMissionChunkData ||
        offer.chunk_count != mission_chunk_count(offer.packed_bytes))
        return false;
    offer_ = offer;
    count_ = offer.chunk_count;
    bytes_.assign(offer.packed_bytes, 0);
    got_.assign(count_, 0);
    active_ = true;
    return true;
}

MissionAssembly::Add MissionAssembly::add(const MissionChunkHead& head, const uint8_t* payload) {
    if (!active_ || head.mission_id != offer_.mission_id || head.index >= count_) return Add::Bad;
    const size_t off = static_cast<size_t>(head.index) * kMissionChunkData;
    const size_t want = bytes_.size() - off < kMissionChunkData ? bytes_.size() - off : kMissionChunkData;
    if (head.len != want) return Add::Bad;
    if (got_[head.index]) return Add::Duplicate;
    std::memcpy(bytes_.data() + off, payload, want);
    got_[head.index] = 1;
    return ++have_ == count_ ? Add::Done : Add::Ok;
}

}  // namespace LazyWorks::SF2Patch::net
