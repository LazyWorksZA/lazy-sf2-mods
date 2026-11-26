// The comparison report (lazysf2-acmi --report): how far what one machine showed of a remote jet (RemoteShown in the
// viewer's stream) was from where that jet really was (the truth stream), on host time. Developer tool, not shipped.
// No Windows or game dependencies. Formats: docs/stream-format.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace LazyWorks::SF2Patch::acmi {

struct ReportOptions {
    int id = -1;  // the remote's id (0 the host, 1..4 a client); -1: the lowest id with RemoteShown in the viewer
    /// <summary>
    /// Set: the truth is the truth file's HostState plus this offset (right, back, up, metres, in the host's heading
    /// frame), for the bot's formation runs. Unset: the remote's own States in the truth file.
    /// </summary>
    bool has_offset = false;
    double offset[3] = {};
    double window = 10.0;  // seconds of host time per row
};

/// <summary>
/// One error figure. For a magnitude (position, angle): median, p95 and max. For a signed component (along, cross,
/// vertical): the median of the signed values, and p95 and max of their absolute values.
/// </summary>
struct ErrStat {
    size_t n = 0;
    double median = 0, p95 = 0, max = 0;
};

struct ReportRow {
    double t0 = 0, t1 = 0;  // host time span of the compared records
    ErrStat pos;            // metres
    ErrStat along, cross, vert;  // metres: + ahead of the truth, + right of its track, + above
    ErrStat ang;            // degrees: the rotation between the shown and the true attitude
    unsigned snaps = 0;     // RemoteShown records with Out Snap
    double coast_s = 0;     // host seconds the receiver coasted (Out Coast; not compared)
    double accel_p95_g = 0, accel_max_g = 0;  // the smoothness figure: acceleration of the shown path (no snaps)
    double truth_accel_max_g = 0;             // the same of the truth at the same times
};

struct Report {
    bool ok = false;
    std::string error;  // why there is no report (ok false)
    int id = -1;
    std::string viewer_label, truth_label;  // "client 1 \"PLAYER\" (F-105D)", from SessionInfo
    std::string truth_source;               // which records were used as the truth
    double viewer_span[2] = {}, truth_span[2] = {};  // host time
    size_t shown = 0;     // RemoteShown records of the id
    size_t compared = 0;  // ... compared with the truth
    size_t outside = 0;   // ... outside the truth's time span
    size_t gaps = 0;      // ... where the truth has a gap over 1 s
    unsigned parked = 0, taken = 0, lost = 0;  // AvatarEvent records of the id
    std::vector<ReportRow> rows;
    ReportRow overall;
};

/// <summary>Builds the report. False (with r.error) when a file is invalid, the id has no records, or the files do not overlap.</summary>
bool make_report(const uint8_t* viewer, size_t viewer_size, const uint8_t* truth, size_t truth_size,
                 const ReportOptions& opt, Report& r);

/// <summary>The report as text, for the console.</summary>
std::string format_report(const Report& r);

}  // namespace LazyWorks::SF2Patch::acmi
