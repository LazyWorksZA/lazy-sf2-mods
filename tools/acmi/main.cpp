// lazysf2-acmi: converts a recorded stream (stream_NNN, client_NNN or botserve_NNN.lzst) to a Tacview ACMI 2.2 text
// file, or compares what one recording showed of a remote jet with another's truth (--report).
// Developer tool, not shipped by the installer.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "acmi/acmi_convert.h"
#include "acmi/net_report.h"
#include "net/protocol.h"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: lazysf2-acmi <stream.lzst> <out.acmi> [--ref-lat <deg>] [--ref-lon <deg>] [--max-rate <hz>] [--ghosts]\n"
                 "       lazysf2-acmi --report <viewer.lzst> <truth.lzst> [--id <n>] [--offset <r,b,u>] [--window <s>]\n"
                 "  --ref-lat/--ref-lon  reference point of the local flat map (default 0 0)\n"
                 "  --max-rate           most samples per second per object (default 30, 0 = all)\n"
                 "  --ghosts             also each jet as the recorder showed it (\"<callsign> as seen by <viewer>\", green)\n"
                 "  --report             how far the viewer's shown jet was from the truth file's, on host time\n"
                 "  --id                 the remote's id (0 the host, 1..4 a client; default: the lowest one shown)\n"
                 "  --offset             truth = the truth file's host jet + metres right, back, up (bot formation runs)\n"
                 "  --window             seconds per report row (default 10)\n");
}

bool parse_num(const char* s, double& v) {
    char* end = nullptr;
    v = std::strtod(s, &end);
    return end && end != s && *end == '\0';
}

bool parse_offset(const char* s, double out[3]) {
    char* end = nullptr;
    for (int i = 0; i < 3; ++i) {
        out[i] = std::strtod(s, &end);
        if (!end || end == s) return false;
        if (i < 2 && *end != ',') return false;
        s = end + (i < 2 ? 1 : 0);
    }
    return *end == '\0';
}

bool read_all(const char* path, std::vector<uint8_t>& bytes) {
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace LazyWorks::SF2Patch;
    acmi::Options opt;
    acmi::ReportOptions ropt;
    bool report = false;
    double id = -1;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        double* target = a == "--ref-lat"    ? &opt.ref_lat
                         : a == "--ref-lon"  ? &opt.ref_lon
                         : a == "--max-rate" ? &opt.max_rate
                         : a == "--id"       ? &id
                         : a == "--window"   ? &ropt.window
                                             : nullptr;
        if (target || a == "--offset") {
            const bool ok = i + 1 < argc && (target ? parse_num(argv[i + 1], *target) : parse_offset(argv[i + 1], ropt.offset));
            if (!ok) {
                std::fprintf(stderr, "bad or missing value for %s\n", a.c_str());
                usage();
                return 2;
            }
            if (!target) ropt.has_offset = true;
            ++i;
        } else if (a == "--ghosts") {
            opt.ghosts = true;
        } else if (a == "--report") {
            report = true;
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (pos.size() != 2 || (report && (id < -1 || id > 65535 || !(ropt.window > 0)))) {
        usage();
        return 2;
    }

    if (report) {
        ropt.id = static_cast<int>(id);
        std::vector<uint8_t> v, t;
        if (!read_all(pos[0], v) || !read_all(pos[1], t)) return 1;
        acmi::Report r;
        const bool ok = acmi::make_report(v.data(), v.size(), t.data(), t.size(), ropt, r);
        std::printf("%s", acmi::format_report(r).c_str());
        return ok ? 0 : 1;
    }

    std::vector<uint8_t> bytes;
    if (!read_all(pos[0], bytes)) return 1;
    std::string text;
    acmi::Info info;
    if (!acmi::convert_stream(bytes.data(), bytes.size(), opt, text, info)) {
        std::fprintf(stderr, "%s: not a valid stream file (%s)\n", pos[0], net::stream_status_name(info.end));
        return 1;
    }
    if (info.other_protocol && info.samples_in == 0) {
        std::fprintf(stderr, "%s: recorded with protocol v%u; this build reads v%u (record it again)\n", pos[0],
                     static_cast<unsigned>(info.other_version), static_cast<unsigned>(net::kVersion));
        return 1;
    }
    if (info.other_protocol)
        std::fprintf(stderr, "warning: %zu records of protocol v%u skipped (this build reads v%u)\n", info.other_protocol,
                     static_cast<unsigned>(info.other_version), static_cast<unsigned>(net::kVersion));
    if (info.end != net::StreamStatus::End)
        std::fprintf(stderr, "warning: the stream stops early (%s); converted what was read\n",
                     net::stream_status_name(info.end));
    if (info.skipped_bad)
        std::fprintf(stderr, "warning: %zu samples skipped (non-finite values or unknown client)\n", info.skipped_bad);

    FILE* o = nullptr;
    fopen_s(&o, pos[1], "wb");
    if (!o || std::fwrite(text.data(), 1, text.size(), o) != text.size()) {
        std::fprintf(stderr, "cannot write %s\n", pos[1]);
        if (o) std::fclose(o);
        return 1;
    }
    std::fclose(o);
    std::printf("%zu records, %zu objects, %zu samples written (%zu read, %zu ghost) -> %s\n", info.records, info.objects,
                info.samples_out, info.samples_in, info.ghost_samples, pos[1]);
    return 0;
}
