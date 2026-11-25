#include "net/stream_format.h"

#include <cstring>

namespace LazyWorks::SF2Patch::net {

size_t encode_stream_header(double start_game_time, const char* theatre, uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(StreamHeader)) return 0;
    StreamHeader h{};
    std::memcpy(h.magic, kStreamMagic, 4);
    h.version = kStreamVersion;
    h.header_len = sizeof(StreamHeader);
    h.start_game_time = start_game_time;
    if (theatre) {
        for (size_t i = 0; i + 1 < kStreamTheatreLen && theatre[i]; ++i) h.theatre[i] = theatre[i];
    }
    std::memcpy(out, &h, sizeof(h));
    return sizeof(h);
}

size_t encode_stream_record(StreamKind kind, double time, const uint8_t* payload, size_t payload_len, uint8_t* out,
                            size_t cap) {
    if (payload_len > kStreamMaxPayload || (payload_len && !payload)) return 0;
    const size_t total = kStreamRecordHeadBytes + payload_len;
    if (!out || cap < total) return 0;
    const uint32_t length = static_cast<uint32_t>(kStreamRecordMin + payload_len);
    std::memcpy(out, &length, 4);
    out[4] = static_cast<uint8_t>(kind);
    std::memcpy(out + 5, &time, 8);
    if (payload_len) std::memcpy(out + kStreamRecordHeadBytes, payload, payload_len);
    return total;
}

StreamStatus StreamReader::read_header(StreamHeader& out) {
    if (have_header_) return StreamStatus::Ok;
    if (failed_ != StreamStatus::Ok) return failed_;
    auto fail = [&](StreamStatus s) {
        failed_ = s;
        return s;
    };
    // magic, version and header_len are the first 8 bytes.
    if (size_ < 8) return fail(StreamStatus::Truncated);
    if (std::memcmp(data_, kStreamMagic, 4) != 0) return fail(StreamStatus::BadMagic);
    uint16_t version, header_len;
    std::memcpy(&version, data_ + 4, 2);
    std::memcpy(&header_len, data_ + 6, 2);
    if (version != kStreamVersion) return fail(StreamStatus::BadVersion);
    if (header_len < sizeof(StreamHeader)) return fail(StreamStatus::BadHeader);
    if (size_ < header_len) return fail(StreamStatus::Truncated);
    StreamHeader h;
    std::memcpy(&h, data_, sizeof(h));
    h.theatre[kStreamTheatreLen - 1] = '\0';
    out = h;
    pos_ = header_len;
    have_header_ = true;
    return StreamStatus::Ok;
}

StreamStatus StreamReader::next(StreamRecord& rec) {
    if (failed_ != StreamStatus::Ok) return failed_;
    auto fail = [&](StreamStatus s) {
        failed_ = s;
        return s;
    };
    if (!have_header_) return fail(StreamStatus::BadHeader);
    const size_t left = size_ - pos_;
    if (left == 0) return fail(StreamStatus::End);
    if (left < 4) return fail(StreamStatus::Truncated);
    uint32_t length;
    std::memcpy(&length, data_ + pos_, 4);
    if (length < kStreamRecordMin || length > kStreamRecordMax) return fail(StreamStatus::BadLength);
    if (left - 4 < length) return fail(StreamStatus::Truncated);
    const uint8_t* p = data_ + pos_ + 4;
    rec.kind = p[0];
    std::memcpy(&rec.time, p + 1, 8);
    rec.payload_len = length - kStreamRecordMin;
    rec.payload = p + 9;
    pos_ += 4 + static_cast<size_t>(length);
    return StreamStatus::Ok;
}

const char* stream_status_name(StreamStatus s) {
    switch (s) {
        case StreamStatus::Ok: return "ok";
        case StreamStatus::End: return "end";
        case StreamStatus::Truncated: return "truncated";
        case StreamStatus::BadMagic: return "bad magic";
        case StreamStatus::BadVersion: return "bad version";
        case StreamStatus::BadHeader: return "bad header";
        case StreamStatus::BadLength: return "bad record length";
    }
    return "?";
}

}  // namespace LazyWorks::SF2Patch::net
