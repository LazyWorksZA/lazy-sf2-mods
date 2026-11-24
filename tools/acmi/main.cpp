// lazysf2-acmi: converts a recorded host stream (stream_NNN.lzst) to a Tacview ACMI 2.2 text file.
// Developer tool, not shipped by the installer.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "acmi/acmi_convert.h"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: lazysf2-acmi <stream.lzst> <out.acmi> [--ref-lat <deg>] [--ref-lon <deg>] [--max-rate <hz>]\n"
                 "  --ref-lat/--ref-lon  reference point of the local flat map (default 0 0)\n"
                 "  --max-rate           most samples per second per object (default 30, 0 = all)\n");
}

bool parse_num(const char* s, double& v) {
    char* end = nullptr;
    v = std::strtod(s, &end);
    return end && end != s && *end == '\0';
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lazysf2;
    acmi::Options opt;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        double* target = a == "--ref-lat" ? &opt.ref_lat : a == "--ref-lon" ? &opt.ref_lon : a == "--max-rate" ? &opt.max_rate : nullptr;
        if (target) {
            if (i + 1 >= argc || !parse_num(argv[i + 1], *target)) {
                std::fprintf(stderr, "bad or missing value for %s\n", a.c_str());
                usage();
                return 2;
            }
            ++i;
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (pos.size() != 2) {
        usage();
        return 2;
    }

    std::vector<uint8_t> bytes;
    {
        FILE* f = nullptr;
        fopen_s(&f, pos[0], "rb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", pos[0]);
            return 1;
        }
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }

    std::string text;
    acmi::Info info;
    if (!acmi::convert_stream(bytes.data(), bytes.size(), opt, text, info)) {
        std::fprintf(stderr, "%s: not a valid stream file (%s)\n", pos[0], net::stream_status_name(info.end));
        return 1;
    }
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
    std::printf("%zu records, %zu objects, %zu samples written (%zu read) -> %s\n", info.records, info.objects,
                info.samples_out, info.samples_in, pos[1]);
    return 0;
}
