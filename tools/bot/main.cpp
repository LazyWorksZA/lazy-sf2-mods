// lazysf2-bot: a headless synthetic client for the host mode, or (--serve) a synthetic host for the client mode.
// Developer tool, not shipped.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bot_client.h"
#include "bot_serve.h"

namespace {

void usage() {
    std::printf(
        "lazysf2-bot --host ADDR:PORT [--callsign BOT1] [--aircraft F-4E]\n"
        "            [--script straight|orbit|breaks|climb|formation] [--offset R,B,U] [--rate HZ] [--duration SECONDS]\n"
        "            [--loss PERCENT] [--jitter MS] [--lag MS] [--log-remote] [--drop] [--log FILE]\n"
        "lazysf2-bot --serve PORT [--bind ADDR] [--callsign BOTHOST] [--aircraft F-4E] [--script straight|orbit|breaks|climb]\n"
        "            [--start X,Y,Z,HEADING] [--lag MS] [--pause-at S --pause-for S] [--record DIR] [--duration SECONDS]\n"
        "            [--log FILE]\n"
        "Client defaults: 127.0.0.1:7777, BOT1, F-4E, straight, 30 Hz, 30 s, no loss, no jitter, no lag.\n"
        "--script formation flies on the host's predicted jet, --offset metres right, back and up in its heading frame\n"
        "(default 150,50,0); before the first host State it flies straight on from the Welcome's join position.\n"
        "--jitter delays each State by a random 0..MS before it is sent, so States can overtake each other: on the\n"
        "sequenced (unreliable) channel a late State is dropped by the host, so jitter also acts as extra loss.\n"
        "--lag holds every packet sent and every packet received for MS (a fixed delay each way).\n"
        "--log-remote logs one line per second per remote jet. --drop exits without Bye (tests the host's 5 s timeout).\n"
        "Client exit code 0 ok, 1 bad arguments, 2 cannot connect (bad address, refused), 3 no ENet connection within\n"
        "4 s, or no Welcome within 15 s of connecting, 4 the host closed the connection or went silent for 4 s.\n"
        "Serve defaults: bind 127.0.0.1, BOTHOST, F-4E, orbit, until Ctrl+C. The jet's States go out at 20 Hz. Without\n"
        "--start (HEADING in degrees) the jet starts 2 km ahead of the client's first State, at its altitude and heading,\n"
        "and a second Welcome puts the client on its wing. --pause-at S --pause-for S freezes the host time S seconds\n"
        "after the jet starts. --record writes DIR\\botserve_NNN.lzst. --lag here delays the handling of what arrives\n"
        "and the jet's States, not the Pongs. Serve exit code 0 ok, 1 bad arguments, 2 cannot listen.\n");
}

bool to_double(const char* s, double* out) {
    char* end = nullptr;
    *out = std::strtod(s, &end);
    return end && end != s && !*end && std::isfinite(*out);
}

/// <summary>"a,b,c" (n numbers, comma separated) into out.</summary>
bool to_doubles(const char* s, double* out, int n) {
    std::string rest = s;
    for (int i = 0; i < n; ++i) {
        const size_t comma = rest.find(',');
        if ((i + 1 < n) != (comma != std::string::npos)) return false;
        const std::string part = i + 1 < n ? rest.substr(0, comma) : rest;
        if (!to_double(part.c_str(), &out[i])) return false;
        if (i + 1 < n) rest = rest.substr(comma + 1);
    }
    return true;
}

FILE* g_logfile = nullptr;
std::atomic<bool> g_cancel{false};

void log_line(const char* line) {
    std::printf("%s\n", line);
    std::fflush(stdout);
    if (g_logfile) {
        std::fprintf(g_logfile, "%s\n", line);
        std::fflush(g_logfile);
    }
}

BOOL WINAPI on_ctrl(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
    g_cancel.store(true);  // the run ends with a Bye
    return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
    namespace bot = LazyWorks::SF2Patch::bot;
    bot::BotConfig cfg;
    bot::ServeConfig scfg;
    cfg.log = log_line;
    scfg.log = log_line;
    cfg.cancel = &g_cancel;
    scfg.cancel = &g_cancel;
    const char* log_path = nullptr;
    bool serve = false, script_given = false;
    bool client_only = false, serve_only = false;  // options of one mode only
    double pause_at = -1, pause_for = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        bool ok = true;
        static const char* const kNeedValue[] = {"--host",  "--callsign", "--aircraft", "--script",   "--rate",
                                                 "--duration", "--loss",   "--jitter",   "--log",      "--lag",
                                                 "--offset", "--serve",    "--bind",     "--start",    "--record",
                                                 "--pause-at", "--pause-for"};
        bool needs_value = false;
        for (const char* o : kNeedValue) needs_value = needs_value || a == o;
        if (needs_value && i + 1 >= argc) {
            std::printf("missing value for %s\n", a.c_str());
            usage();
            return 1;
        }
        if (a == "--drop") {
            cfg.drop = true;
            client_only = true;
        } else if (a == "--log-remote") {
            cfg.log_remote = true;
            client_only = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--host" && (v = val())) {
            cfg.host = v;
            client_only = true;
        } else if (a == "--serve" && (v = val())) {
            double port = 0;
            ok = to_double(v, &port) && port >= 1 && port <= 65535 && port == std::floor(port);
            scfg.port = static_cast<uint16_t>(port);
            serve = true;
        } else if (a == "--bind" && (v = val())) {
            scfg.bind = v;
            serve_only = true;
        } else if (a == "--callsign" && (v = val())) {
            cfg.callsign = scfg.callsign = v;
        } else if (a == "--aircraft" && (v = val())) {
            cfg.aircraft = scfg.aircraft = v;
        } else if (a == "--script" && (v = val())) {
            ok = bot::parse_script(v, &cfg.script);
            scfg.script = cfg.script;
            script_given = true;
        } else if (a == "--rate" && (v = val())) {
            ok = to_double(v, &cfg.rate);
            client_only = true;
        } else if (a == "--duration" && (v = val())) {
            ok = to_double(v, &cfg.duration);
            scfg.duration = cfg.duration;
        } else if (a == "--loss" && (v = val())) {
            ok = to_double(v, &cfg.loss_pct);
            client_only = true;
        } else if (a == "--jitter" && (v = val())) {
            ok = to_double(v, &cfg.jitter_ms);
            client_only = true;
        } else if (a == "--lag" && (v = val())) {
            ok = to_double(v, &cfg.lag_ms);
            scfg.lag_ms = cfg.lag_ms;
        } else if (a == "--offset" && (v = val())) {
            ok = to_doubles(v, cfg.offset, 3);
            client_only = true;
        } else if (a == "--start" && (v = val())) {
            double s[4];
            ok = to_doubles(v, s, 4);
            for (int k = 0; k < 3; ++k) scfg.start[k] = s[k];
            scfg.start_heading = s[3] * 3.14159265358979323846 / 180.0;
            scfg.has_start = true;
            serve_only = true;
        } else if (a == "--record" && (v = val())) {
            const int n = MultiByteToWideChar(CP_ACP, 0, v, -1, nullptr, 0);
            std::wstring w(n > 0 ? static_cast<size_t>(n) : 1, L'\0');
            ok = n > 1 && MultiByteToWideChar(CP_ACP, 0, v, -1, w.data(), n) == n;
            w.resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
            scfg.record_dir = w;
            serve_only = true;
        } else if (a == "--pause-at" && (v = val())) {
            ok = to_double(v, &pause_at) && pause_at >= 0;
            serve_only = true;
        } else if (a == "--pause-for" && (v = val())) {
            ok = to_double(v, &pause_for) && pause_for >= 0;
            serve_only = true;
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
    if (serve ? client_only : serve_only) {
        std::printf(serve ? "--host, --rate, --loss, --jitter, --offset, --log-remote and --drop are client options\n"
                          : "--bind, --start, --record and --pause-* need --serve\n");
        usage();
        return 1;
    }
    if (serve && ((pause_at >= 0) != (pause_for >= 0))) {
        std::printf("--pause-at and --pause-for go together\n");
        usage();
        return 1;
    }
    if (serve && pause_at >= 0) {
        scfg.pause_at = pause_at;
        scfg.pause_for = pause_for;
    }
    if (serve && !script_given) scfg.script = bot::Script::Orbit;
    if (serve && scfg.script == bot::Script::Formation) {
        std::printf("--serve cannot fly formation: it is the lead\n");
        return 1;
    }
    if (log_path && fopen_s(&g_logfile, log_path, "w") != 0) {
        std::printf("cannot open the log file %s\n", log_path);
        return 1;
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    int code = 0;
    if (serve) {
        code = bot::run_serve(scfg).code;
    } else {
        code = bot::run_bot(cfg).code;
    }
    if (g_logfile) std::fclose(g_logfile);
    return code;
}
