// Converts a recorded stream (*.lzst of the host, a client or the bot's --serve; docs/stream-format.md) to Tacview
// ACMI 2.2 text.
// Developer tool, not shipped. No Windows or game dependencies. Spec: Tacview "ACMI telemetry file format".
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/stream_format.h"

namespace LazyWorks::SF2Patch::acmi {

struct Options {
    double ref_lat = 0;    // degrees; the origin of the local flat mapping
    double ref_lon = 0;    // degrees
    double max_rate = 30;  // samples per second per object at most; 0 or less means no limit
    bool ghosts = false;   // also each RemoteShown track, as its own object ("<callsign> as seen by <viewer>")
};

struct Info {
    bool header_ok = false;
    net::StreamStatus end = net::StreamStatus::Ok;  // why reading stopped (End is normal)
    size_t records = 0;       // records read
    size_t samples_in = 0;    // usable State samples read
    size_t samples_out = 0;   // transform lines written
    size_t skipped_bad = 0;   // samples dropped (non-finite values, or a client that never joined)
    size_t objects = 0;       // objects created
    size_t ghost_samples = 0; // transform lines written for RemoteShown tracks (with Options::ghosts)
};

/// <summary>
/// Fills `out` with the ACMI text. Returns false only when the header is invalid (nothing is written then).
/// A truncated or corrupt tail is not an error: info.end says what happened, and what was read is converted.
/// </summary>
bool convert_stream(const uint8_t* data, size_t size, const Options& opt, std::string& out, Info& info);

}  // namespace LazyWorks::SF2Patch::acmi
