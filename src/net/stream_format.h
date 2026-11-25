// The on-disk format of a recorded host stream (*.lzst), and a defensive reader for it.
// No game or Windows dependencies. Documented in docs/stream-format.md; keep the two in step.
// All fields are little-endian and packed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace LazyWorks::SF2Patch::net {

inline constexpr char kStreamMagic[4] = {'L', 'Z', 'S', 'T'};
inline constexpr uint16_t kStreamVersion = 1;
inline constexpr size_t kStreamTheatreLen = 32;  // bytes in the header's theatre field, NUL padded
inline constexpr uint32_t kStreamRecordMin = 9;  // kind (1) + time (8): a record with no payload
inline constexpr uint32_t kStreamMaxPayload = 96;
inline constexpr uint32_t kStreamRecordMax = kStreamRecordMin + kStreamMaxPayload;  // largest valid `length`
inline constexpr uint64_t kStreamMaxFileBytes = 512ull * 1024 * 1024;

/// <summary>
/// What a record holds. The payload of the first four is the protocol message exactly as it was on the wire
/// (see protocol.h); the other two are defined below. A reader skips kinds it does not know.
/// </summary>
enum class StreamKind : uint8_t {
    StateReceived = 1,  // StateMsg from a client (client_id 1..4)
    HostState = 2,      // StateMsg of the host player, client_id 0, once per player step
    WelcomeSent = 3,    // WelcomeMsg
    ByeReceived = 4,    // ByeMsg from a client
    ClientJoined = 5,   // ClientJoinedPayload
    ClientLeft = 6,     // ClientLeftPayload
};

enum class LeaveReason : uint8_t {
    Left = 0,       // Bye or disconnect from the client
    Timeout = 1,    // no packets for 5 s
    HostEnded = 2,  // avatar destroyed, the player changed, or the host shut down
    NoWelcome = 3,  // the Welcome could not be queued
};

#pragma pack(push, 1)
struct StreamHeader {
    char magic[4];
    uint16_t version;
    uint16_t header_len;     // bytes in the whole header, including this field; a reader skips to here
    double start_game_time;  // host game time when the file was opened
    char theatre[kStreamTheatreLen];  // host theatre name, or all NUL when unknown
};
struct ClientJoinedPayload {
    uint16_t client_id;
    char callsign[16];  // NUL padded, printable ASCII
    char aircraft[32];  // NUL padded, printable ASCII
};
struct ClientLeftPayload {
    uint16_t client_id;
    uint8_t reason;  // LeaveReason
};
#pragma pack(pop)

static_assert(sizeof(StreamHeader) == 48);
static_assert(sizeof(ClientJoinedPayload) == 50);
static_assert(sizeof(ClientLeftPayload) == 3);

/// <summary>
/// Record layout: uint32 length (the bytes that follow it: kind + time + payload, 9 to 105), uint8 kind,
/// double host game time, then length - 9 payload bytes.
/// </summary>
inline constexpr size_t kStreamRecordHeadBytes = 4 + 1 + 8;  // length + kind + time
inline constexpr size_t kStreamRecordTotalMax = 4 + kStreamRecordMax;

/// <summary>Writes a header (theatre may be null) into out. Returns sizeof(StreamHeader), or 0 if cap is too small.</summary>
size_t encode_stream_header(double start_game_time, const char* theatre, uint8_t* out, size_t cap);
/// <summary>
/// Writes one record into out. Returns its total size (4 + length), or 0 if payload_len is above
/// kStreamMaxPayload or cap is too small.
/// </summary>
size_t encode_stream_record(StreamKind kind, double time, const uint8_t* payload, size_t payload_len, uint8_t* out,
                            size_t cap);

enum class StreamStatus {
    Ok,
    End,         // the data ended exactly at a record boundary
    Truncated,   // the data ends inside the header or a record: what came before is good
    BadMagic,
    BadVersion,
    BadHeader,   // header_len smaller than the header we know, or next() before read_header()
    BadLength,   // a record length outside 9..105: nothing after it can be trusted
};

struct StreamRecord {
    uint8_t kind = 0;  // StreamKind, or a kind this reader does not know
    double time = 0;
    const uint8_t* payload = nullptr;  // points into the reader's data
    uint32_t payload_len = 0;
};

/// <summary>Parses a stream held in memory. It never reads outside [data, data + size), and never allocates.</summary>
class StreamReader {
public:
    StreamReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    /// <summary>Call once, first. The header is only written on Ok.</summary>
    StreamStatus read_header(StreamHeader& out);
    /// <summary>
    /// Next record. Ok fills rec; anything else means stop (End is the normal end). After a failure the
    /// reader keeps returning the same status.
    /// </summary>
    StreamStatus next(StreamRecord& rec);

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool have_header_ = false;
    StreamStatus failed_ = StreamStatus::Ok;
};

const char* stream_status_name(StreamStatus s);

}  // namespace LazyWorks::SF2Patch::net
