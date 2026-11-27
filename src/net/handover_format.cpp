#include "net/handover_format.h"

#include <miniz.h>

#include <cstring>

namespace LazyWorks::SF2Patch::net {

namespace {

template <class T>
size_t enc(const T& in, WorldType t, uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(T)) return 0;
    T m = in;
    m.h.magic = kMagic;
    m.h.version = kVersion;
    m.h.type = static_cast<uint8_t>(t);
    std::memcpy(out, &m, sizeof(T));
    return sizeof(T);
}

template <class T>
Status dec(const uint8_t* data, size_t len, WorldType t, T& out) {
    if (!data || len < sizeof(Header)) return Status::Truncated;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return Status::BadMagic;
    if (h.version != kVersion) return Status::BadVersion;
    if (h.type != static_cast<uint8_t>(t)) return Status::BadType;
    if (len < sizeof(T)) return Status::Truncated;
    if (len > sizeof(T)) return Status::Oversized;
    std::memcpy(&out, data, sizeof(T));
    return Status::Ok;
}

template <class T>
void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}

}  // namespace

std::vector<uint8_t> serialize(const HandoverSnapshot& s) {
    std::vector<uint8_t> v;
    HandoverHead h = s.head;
    h.aircraft = static_cast<uint16_t>(s.aircraft.size());
    h.vehicles = static_cast<uint16_t>(s.vehicles.size());
    h.flights = static_cast<uint16_t>(s.flights.size());
    v.reserve(sizeof(h) + s.aircraft.size() * sizeof(HandoverAircraft) + s.vehicles.size() * sizeof(HandoverVehicle) +
              s.flights.size() * sizeof(HandoverFlight));
    put(v, h);
    for (const auto& a : s.aircraft) put(v, a);
    for (const auto& x : s.vehicles) put(v, x);
    for (const auto& f : s.flights) put(v, f);
    return v;
}

bool deserialize(const uint8_t* data, size_t len, HandoverSnapshot* out) {
    if (!data || len < sizeof(HandoverHead)) return false;
    HandoverSnapshot s;
    std::memcpy(&s.head, data, sizeof(s.head));
    const size_t want = sizeof(HandoverHead) + s.head.aircraft * sizeof(HandoverAircraft) +
                        s.head.vehicles * sizeof(HandoverVehicle) + s.head.flights * sizeof(HandoverFlight);
    if (len != want) return false;
    const uint8_t* p = data + sizeof(HandoverHead);
    s.aircraft.resize(s.head.aircraft);
    s.vehicles.resize(s.head.vehicles);
    s.flights.resize(s.head.flights);
    if (!s.aircraft.empty()) std::memcpy(s.aircraft.data(), p, s.aircraft.size() * sizeof(HandoverAircraft));
    p += s.aircraft.size() * sizeof(HandoverAircraft);
    if (!s.vehicles.empty()) std::memcpy(s.vehicles.data(), p, s.vehicles.size() * sizeof(HandoverVehicle));
    p += s.vehicles.size() * sizeof(HandoverVehicle);
    if (!s.flights.empty()) std::memcpy(s.flights.data(), p, s.flights.size() * sizeof(HandoverFlight));
    *out = std::move(s);
    return true;
}

size_t encode(const HandoverBeginMsg& m, uint8_t* o, size_t c) { return enc(m, WorldType::HandoverBegin, o, c); }
Status decode(const uint8_t* d, size_t n, HandoverBeginMsg& out) { return dec(d, n, WorldType::HandoverBegin, out); }
size_t encode(const HandoverChunkMsg& m, uint8_t* o, size_t c) { return enc(m, WorldType::HandoverChunk, o, c); }
Status decode(const uint8_t* d, size_t n, HandoverChunkMsg& out) {
    HandoverChunkMsg m;
    const Status s = dec(d, n, WorldType::HandoverChunk, m);
    if (s != Status::Ok) return s;
    if (m.len == 0 || m.len > kHandoverChunkData) return Status::BadType;
    out = m;
    return Status::Ok;
}
size_t encode(const HandoverDoneMsg& m, uint8_t* o, size_t c) { return enc(m, WorldType::HandoverDone, o, c); }
Status decode(const uint8_t* d, size_t n, HandoverDoneMsg& out) { return dec(d, n, WorldType::HandoverDone, out); }

bool pack_handover(const std::vector<uint8_t>& raw, std::vector<uint8_t>* packed, uint32_t* crc) {
    if (raw.empty() || raw.size() > kHandoverMaxRaw) return false;
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<uint8_t> out(cap);
    if (mz_compress2(out.data(), &cap, raw.data(), static_cast<mz_ulong>(raw.size()), MZ_BEST_COMPRESSION) != MZ_OK)
        return false;
    if (cap > kHandoverMaxPacked) return false;
    out.resize(cap);
    *crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, out.data(), out.size()));
    *packed = std::move(out);
    return true;
}

bool unpack_handover(const std::vector<uint8_t>& packed, uint32_t crc, uint32_t raw_size, std::vector<uint8_t>* raw) {
    if (packed.empty() || raw_size == 0 || raw_size > kHandoverMaxRaw) return false;
    if (static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, packed.data(), packed.size())) != crc) return false;
    std::vector<uint8_t> out(raw_size);
    mz_ulong got = raw_size;
    if (mz_uncompress(out.data(), &got, packed.data(), static_cast<mz_ulong>(packed.size())) != MZ_OK || got != raw_size)
        return false;
    *raw = std::move(out);
    return true;
}

bool HandoverAssembly::begin(const HandoverBeginMsg& b) {
    *this = HandoverAssembly{};
    if (b.packed == 0 || b.packed > kHandoverMaxPacked || b.raw == 0 || b.raw > kHandoverMaxRaw ||
        b.chunks != (b.packed + kHandoverChunkData - 1) / kHandoverChunkData)
        return false;
    offer_ = b;
    count_ = b.chunks;
    bytes_.assign(b.packed, 0);
    got_.assign(count_, 0);
    active_ = true;
    return true;
}

bool HandoverAssembly::add(const HandoverChunkMsg& c) {
    if (!active_ || c.epoch != offer_.epoch || c.index >= count_ || got_[c.index]) return complete();
    const size_t off = static_cast<size_t>(c.index) * kHandoverChunkData;
    const size_t want = bytes_.size() - off < kHandoverChunkData ? bytes_.size() - off : kHandoverChunkData;
    if (c.len != want) return complete();
    std::memcpy(bytes_.data() + off, c.data, want);
    got_[c.index] = 1;
    ++have_;
    return complete();
}

}  // namespace LazyWorks::SF2Patch::net
