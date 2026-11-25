// lazysf2-bot: a headless synthetic client for the host mode. Developer tool, not shipped.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bot_client.h"

namespace {

void usage() {
    std::printf(
        "lazysf2-bot --host ADDR:PORT [--callsign BOT1] [--aircraft F-4E] [--script straight|orbit|breaks|climb]\n"
        "            [--rate HZ] [--duration SECONDS] [--loss PERCENT] [--jitter MS] [--drop] [--log FILE]\n"
        "Defaults: 127.0.0.1:7777, BOT1, F-4E, straight, 30 Hz, 30 s, no loss, no jitter.\n"
        "--jitter delays each State by a random 0..MS before it is sent, so States can overtake each other: on the\n"
        "sequenced (unreliable) channel a late State is dropped by the host, so jitter also acts as extra loss.\n"
        "--drop exits without Bye (tests the host's 5 s timeout). Exit code 0 ok, 1 bad arguments, 2 cannot connect\n"
        "(bad address, refused), 3 no ENet connection within 4 s, or no Welcome within 15 s of connecting, 4 the host\n"
        "closed the connection or went silent for 4 s.\n");
}

bool to_double(const char* s, double* out) {
    char* end = nullptr;
    *out = std::strtod(s, &end);
    return end && end != s && !*end && std::isfinite(*out);
}

FILE* g_logfile = nullptr;

void log_line(const char* line) {
    std::printf("%s\n", line);
    std::fflush(stdout);
    if (g_logfile) {
        std::fprintf(g_logfile, "%s\n", line);
        std::fflush(g_logfile);
    }
}

}  // namespace

int main(int argc, char** argv) {
    lazysf2::bot::BotConfig cfg;
    cfg.log = log_line;
    const char* log_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        bool ok = true;
        static const char* const kNeedValue[] = {"--host", "--callsign", "--aircraft", "--script",
                                                 "--rate", "--duration", "--loss",     "--jitter", "--log"};
        bool needs_value = false;
        for (const char* o : kNeedValue) needs_value = needs_value || a == o;
        if (needs_value && i + 1 >= argc) {
            std::printf("missing value for %s\n", a.c_str());
            usage();
            return 1;
        }
        if (a == "--drop") {
            cfg.drop = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--host" && (v = val())) {
            cfg.host = v;
        } else if (a == "--callsign" && (v = val())) {
            cfg.callsign = v;
        } else if (a == "--aircraft" && (v = val())) {
            cfg.aircraft = v;
        } else if (a == "--script" && (v = val())) {
            ok = lazysf2::bot::parse_script(v, &cfg.script);
        } else if (a == "--rate" && (v = val())) {
            ok = to_double(v, &cfg.rate);
        } else if (a == "--duration" && (v = val())) {
            ok = to_double(v, &cfg.duration);
        } else if (a == "--loss" && (v = val())) {
            ok = to_double(v, &cfg.loss_pct);
        } else if (a == "--jitter" && (v = val())) {
            ok = to_double(v, &cfg.jitter_ms);
        } else if (a == "--log" && (v = val())) {
            log_path = v;
        } else {
            ok = false;
        }
        if (!ok) {
            std::printf("bad argument: %s\n", a.c_str());
            usage();
            return 1;
        }
    }
    if (log_path && fopen_s(&g_logfile, log_path, "w") != 0) {
        std::printf("cannot open the log file %s\n", log_path);
        return 1;
    }
    const lazysf2::bot::BotResult res = lazysf2::bot::run_bot(cfg);
    if (g_logfile) std::fclose(g_logfile);
    return res.code;
}
