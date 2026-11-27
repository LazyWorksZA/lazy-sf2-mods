// lazysf2-bot: a headless synthetic client for the host mode, or (--serve) a synthetic host for the client mode.
// Developer tool, not shipped.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <thread>
#include <vector>

#include "bot_client.h"
#include "bot_serve.h"
#include "net/mission_fetch.h"
#include "hooks/world_files.h"
#include "net/world_proto.h"

namespace {

void usage() {
    std::printf(
        "lazysf2-bot --host ADDR:PORT [--callsign BOT1] [--aircraft F-4E]\n"
        "            [--script straight|orbit|breaks|climb|formation] [--offset R,B,U] [--rate HZ] [--duration SECONDS]\n"
        "            [--loss PERCENT] [--jitter MS] [--lag MS] [--log-remote] [--drop] [--world-join] [--log FILE]\n"
        "lazysf2-bot --serve PORT [--bind ADDR] [--callsign BOTHOST] [--aircraft F-4E] [--script straight|orbit|breaks|climb]\n"
        "            [--start X,Y,Z,HEADING] [--lag MS] [--pause-at S --pause-for S] [--record DIR] [--duration SECONDS]\n"
        "            [--mission world_NNN.msn [--seats MEMBERS,HOST_MEMBER]] [--world world_NNN.manifest.txt]\n"
        "            [--world-event destroyed|awake|asleep,A|G,SLOT,MEMBER,SECONDS ...] [--jet none|ab|guns|ab+guns]\n"
        "            [--hit A|G,SLOT,MEMBER,EVERY_S] [--launch WEAPON,GUIDED,A|G|N,SLOT,MEMBER,EVERY_S]\n"
        "            [--stores [WEAPON]] [--damage [A,SLOT,MEMBER]] [--clock S] [--seat-state] [--log FILE]\n"
        "lazysf2-bot --fetch ADDR:PORT --out DIR [--duration SECONDS]\n"
        "lazysf2-bot --lobby ADDR:PORT --content HEX [--callsign BOT1] [--seat N] [--duration SECONDS] [--log FILE]\n"
        "Every mode needs the session password: --password TEXT, or LAZYSF2_PASSWORD (the host proves it by challenge;\n"
        "the password never travels).\n"
        "Client defaults: 127.0.0.1:7777, BOT1, F-4E, straight, 30 Hz, 30 s, no loss, no jitter, no lag.\n"
        "--script formation flies on the host's predicted jet, --offset metres right, back and up in its heading frame\n"
        "(default 150,50,0); before the first host State it flies straight on from the Welcome's join position.\n"
        "--jitter delays each State by a random 0..MS before it is sent, so States can overtake each other: on the\n"
        "sequenced (unreliable) channel a late State is dropped by the host, so jitter also acts as extra loss.\n"
        "--lag holds every packet sent and every packet received for MS (a fixed delay each way).\n"
        "--log-remote logs one line per second per remote jet. --drop exits without Bye (tests the host's 5 s timeout).\n"
        "--world-join joins a shared-world host: asks for its mission (a seat tag), then Hello with that tag, and counts\n"
        "the world stream and events it gets.\n"
        "--lobby joins a host's lobby as a pilot, as the game's join screen does: it shows in the roster with its\n"
        "callsign, asks for seat N (1-based, as the lobby shows it) once the host has a mission, presses Ready for that\n"
        "mission (again after each new one), logs each roster change, and when the host launches follows it as a\n"
        "--world-join client with the lobby's tag for --duration (default 30 s), then leaves. --content is the host's\n"
        "content hash (its lazysf2.log: \"lobby: content hash 0x...\"); another value is kept out. Exit code 0 ok\n"
        "(the flight's code if it flew), 2 refused or stopped, 3 no roster within 15 s.\n"
        "Client exit code 0 ok, 1 bad arguments, 2 cannot connect (bad address, refused), 3 no ENet connection within\n"
        "4 s, or no Welcome within 60 s of the Hello, 4 the host closed the connection or went silent for 4 s.\n"
        "Serve defaults: bind 127.0.0.1, BOTHOST, F-4E, orbit, until Ctrl+C. The jet's States go out at 20 Hz. Without\n"
        "--start (HEADING in degrees) the jet starts 2 km ahead of the client's first State, at its altitude and heading,\n"
        "and a second Welcome puts the client on its wing. --pause-at S --pause-for S freezes the host time S seconds\n"
        "after the jet starts. --record writes DIR\\botserve_NNN.lzst. --lag here delays the handling of what arrives\n"
        "and the jet's States, not the Pongs. --jet ab sends the jet's controls (AvatarState) on a 14 s\n"
        "cycle: idle 4 s, Mil 4 s, full afterburner 4 s, engines off 2 s; guns fires the primary gun 1 s in every 4 s.\n"
        "--hit A,SLOT,MEMBER,EVERY_S sends a scripted 20 kJ gun hit on that jet (the client's seat) every EVERY_S s.\n"
        "--launch WEAPON,GUIDED,A|G|N,SLOT,MEMBER,EVERY_S releases WEAPON (a weapon type name, GUIDED 0 or 1) from the\n"
        "jet's station 0 at that target (N: none) every EVERY_S s; the jet's own key is its seat in the --hit slot.\n"
        "--stores sends the jet's loadout (station 1: WEAPON x2, default AIM-9J) every 10 s and its consumables (fuel,\n"
        "rounds, chaff, flares) every 2 s. --damage plays a damage story on the jet (hit 3 s, part lost 6 s, fire 8 s,\n"
        "ejection 10 s, destroyed 12 s; the attacker A,SLOT,MEMBER if given) and 2 flares every 5 s. --clock S sends\n"
        "the world owner's mission clock (S seconds at the Welcome, then running) every 2 s. --seat-state sends the\n"
        "client its seat's state as a late joiner gets it (airborne 3 km ahead, 2,000 kg fuel, an AIM-9J).\n"
        "Serve exit code 0 ok, 1 bad arguments, 2 cannot listen.\n");
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
std::atomic<bool> g_done{false};  // the run has returned (its Bye is out)

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
    // Closing the console window ends the process as soon as this returns: wait (on this handler thread) for the run
    // to send its Bye, within the system's 5 s limit.
    if (type == CTRL_CLOSE_EVENT)
        for (int i = 0; i < 450 && !g_done.load(); ++i) Sleep(10);
    return TRUE;
}

}  // namespace

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && n <= static_cast<long>(LazyWorks::SF2Patch::net::kMissionMaxRaw);
    if (ok) {
        out->resize(static_cast<size_t>(n));
        ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    }
    std::fclose(f);
    return ok;
}

bool write_file(const std::string& path, const void* data, size_t n) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const bool ok = std::fwrite(data, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

// world_NNN.msn -> world_NNN.sidecar.txt (the mod's companion rule).
std::string sidecar_path(const std::string& msn) {
    std::string base = msn;
    if (base.size() >= 4 && _stricmp(base.c_str() + base.size() - 4, ".msn") == 0) base.resize(base.size() - 4);
    return base + ".sidecar.txt";
}

void fetch_log(const char* line) { log_line(line); }

// The aircraft of a world manifest, for --world: every live aircraft member except member 0 of the player's slot (the
// host's own jet, which travels as Phase 4 States).
bool load_world(const char* path, std::vector<LazyWorks::SF2Patch::bot::ServeConfig::WorldObj>* out) {
    namespace hk = LazyWorks::SF2Patch::hooks;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;
    char line[512];
    int player_slot = -1;
    std::vector<hk::ManifestEntry> es;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        unsigned ps = 0;
        if (sscanf_s(s.c_str(), "H player=A %u.", &ps) == 1) player_slot = static_cast<int>(ps);
        hk::ManifestEntry e;
        if (hk::parse_manifest_line(s, &e) && e.kind == 'A' && e.state == hk::ManifestEntry::Object && e.has_pose)
            es.push_back(e);
    }
    std::fclose(f);
    for (const auto& e : es) {
        if (e.a == player_slot && e.b == 0) continue;
        LazyWorks::SF2Patch::bot::ServeConfig::WorldObj o;
        o.kind = 1;
        o.a = static_cast<uint16_t>(e.a);
        o.b = static_cast<uint16_t>(e.b);
        o.pos[0] = e.pos[0], o.pos[1] = e.pos[1], o.pos[2] = e.pos[2];
        o.heading = e.heading;
        out->push_back(o);
    }
    char b[160];
    std::snprintf(b, sizeof(b), "bot: world from %s: %zu aircraft (player slot %d)", path, out->size(), player_slot);
    log_line(b);
    return !out->empty();
}

// --lobby: a pilot in a host's lobby (roster, seat, Ready), following the host's Launch as a world-join client.
int run_lobby(const std::string& spec, uint32_t content, int seat, LazyWorks::SF2Patch::bot::BotConfig cfg) {
    namespace net = LazyWorks::SF2Patch::net;
    namespace bot = LazyWorks::SF2Patch::bot;
    const size_t colon = spec.rfind(':');
    const int port = colon == std::string::npos ? 0 : std::atoi(spec.c_str() + colon + 1);
    if (colon == std::string::npos || port <= 0 || port > 65535) return 1;
    net::MissionFetch fetch;
    const net::AuthClient auth(cfg.password);
    const uint32_t tag = (GetTickCount() ^ (GetCurrentProcessId() << 12)) | 1u;
    fetch.enable_lobby(cfg.callsign.c_str(), content);
    if (!fetch.start(spec.substr(0, colon).c_str(), static_cast<uint16_t>(port), tag, auth, fetch_log, 1000)) return 2;
    char b[300];
    std::snprintf(b, sizeof(b), "bot lobby: joining %s as \"%s\" (tag 0x%08x, content 0x%08x)", spec.c_str(),
                  cfg.callsign.c_str(), static_cast<unsigned>(tag), static_cast<unsigned>(content));
    log_line(b);
    uint32_t ready_for = 0, flown = 0, shown_id = 0xffffffffu;
    int shown_seat = -2, shown_count = -1;
    uint32_t shown_launch = 0xffffffffu;
    bool asked = false, kept_out = false;
    int code = 0;
    const ULONGLONG give_up = GetTickCount64() + 600000;  // ten minutes in the lobby at most
    const ULONGLONG no_host = GetTickCount64() + 15000;   // ... and 15 s without a roster
    bool have_roster = false;
    while (!g_cancel.load() && GetTickCount64() < give_up) {
        if (!have_roster && GetTickCount64() > no_host) {
            log_line("bot lobby: no roster from the host in 15 s");
            code = 3;
            break;
        }
        if (fetch.state() != net::MissionFetch::State::Running || fetch.wrong_password() || fetch.host_unknown()) {
            log_line(fetch.wrong_password() ? "bot lobby: the host refused the session password"
                                            : "bot lobby: the fetch stopped (no host?)");
            code = 2;
            break;
        }
        net::LobbyRosterMsg r{};
        uint32_t rtt = 0;
        if (fetch.roster(&r, &rtt)) {
            have_roster = true;
            if (r.result == net::kLobbyContent) {
                if (!kept_out) log_line("bot lobby: the host's content hash differs; kept out (check --content)");
                kept_out = true;
            }
            const int you = r.your_seat == net::kNoSeat ? -1 : r.your_seat;
            if (r.mission_id != shown_id || you != shown_seat || r.count != shown_count || r.launch_seq != shown_launch) {
                shown_id = r.mission_id;
                shown_seat = you;
                shown_count = r.count;
                shown_launch = r.launch_seq;
                std::string rows;
                for (size_t i = 0; i < r.count; ++i) {
                    const net::LobbyRow& row = r.rows[i];
                    char one[64];
                    std::snprintf(one, sizeof(one), "%s#%d %s%s", i ? ", " : "", row.seat == net::kNoSeat ? 0 : row.seat + 1,
                                  row.callsign[0] ? row.callsign : "AI",
                                  (row.flags & net::kLobbyRowHost) ? " (host)" : (row.flags & net::kLobbyRowReady) ? " (ready)" : "");
                    rows += one;
                }
                std::snprintf(b, sizeof(b), "bot lobby: mission 0x%08x, %u seats, our seat #%d, launch %u%s, ping %u ms: %s",
                              static_cast<unsigned>(r.mission_id), static_cast<unsigned>(r.members), you + 1,
                              static_cast<unsigned>(r.launch_seq & ~net::kLaunchFlying),
                              (r.launch_seq & net::kLaunchFlying) ? " (flying)" : "", static_cast<unsigned>(rtt), rows.c_str());
                log_line(b);
            }
            if (seat > 0 && !asked && r.members > 0 && r.result != net::kLobbyContent) {
                asked = true;
                fetch.want_seat(seat - 1);
                std::snprintf(b, sizeof(b), "bot lobby: asking for seat #%d", seat);
                log_line(b);
            }
            int ws = -1;
            uint8_t wr = 0;
            if (fetch.wish_answer(&ws, &wr)) {
                std::snprintf(b, sizeof(b), "bot lobby: seat #%d %s", ws + 1, wr == net::kLobbyOk ? "granted" : "refused");
                log_line(b);
            }
            if (r.mission_id && you >= 0 && ready_for != r.mission_id && r.result != net::kLobbyContent) {
                ready_for = r.mission_id;
                fetch.set_ready(true, r.mission_id);
                std::snprintf(b, sizeof(b), "bot lobby: Ready for mission 0x%08x", static_cast<unsigned>(r.mission_id));
                log_line(b);
            }
            const uint32_t launch = r.launch_seq & ~net::kLaunchFlying;
            if ((r.launch_seq & net::kLaunchFlying) && launch != flown && ready_for == r.mission_id && you >= 0) {
                flown = launch;
                std::snprintf(b, sizeof(b), "bot lobby: the host launched (Launch %u); following as seat #%d",
                              static_cast<unsigned>(launch), you + 1);
                log_line(b);
                cfg.host = spec;
                cfg.world_join = true;
                cfg.world_tag = tag;
                const bot::BotResult res = bot::run_bot(cfg);
                std::snprintf(b, sizeof(b), "bot lobby: the flight ended (code %d); back in the lobby", res.code);
                log_line(b);
                code = res.code;
                break;  // one flight per run
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fetch.leave(1000);
    fetch.stop();
    log_line("bot lobby: left");
    return code;
}

// --fetch: asks a host for its mission, as the game's join does, and writes DIR\world_fetched.msn and its sidecar.
int run_fetch(const std::string& spec, const std::string& dir, double duration, const std::string& password) {
    namespace net = LazyWorks::SF2Patch::net;
    const size_t colon = spec.rfind(':');
    const int port = colon == std::string::npos ? 0 : std::atoi(spec.c_str() + colon + 1);
    if (colon == std::string::npos || port <= 0 || port > 65535 || dir.empty()) return 1;
    net::MissionFetch fetch;
    const net::AuthClient auth(password);
    const uint32_t tag = (GetTickCount() ^ (GetCurrentProcessId() << 12)) | 1u;
    if (!fetch.start(spec.substr(0, colon).c_str(), static_cast<uint16_t>(port), tag, auth, fetch_log)) return 2;
    const double limit = duration > 0 ? duration : 60.0;
    std::vector<uint8_t> packed, msn;
    net::MissionOfferMsg offer{};
    bool got = false;
    for (int i = 0; i < static_cast<int>(limit * 50) && !g_cancel.load() && !got; ++i) {
        if (fetch.state() != net::MissionFetch::State::Running) break;
        got = fetch.take(&packed, &offer);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    fetch.stop();
    std::string sidecar;
    const char* why = "";
    if (!got) {
        log_line(fetch.wrong_password() ? "bot fetch: the host refused the session password" : "bot fetch: no mission received");
        return 3;
    }
    if (!net::unpack_mission(packed, offer.mission_id, offer.raw_bytes, &sidecar, &msn, &why)) {
        char b[160];
        std::snprintf(b, sizeof(b), "bot fetch: the mission is unusable (%s)", why);
        log_line(b);
        return 3;
    }
    const std::string out = dir + "\\world_fetched.msn";
    if (!write_file(out, msn.data(), msn.size()) || !write_file(sidecar_path(out), sidecar.data(), sidecar.size())) {
        log_line("bot fetch: cannot write the files");
        return 3;
    }
    char b[400];
    std::snprintf(b, sizeof(b), "bot fetch: wrote %s (%zu bytes) and its sidecar; seat %u assigned", out.c_str(),
                  msn.size(), static_cast<unsigned>(offer.seat));
    log_line(b);
    return 0;
}

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
    std::string fetch_spec, fetch_out, mission_path, password, lobby_spec;
    uint32_t lobby_content = 0;
    int lobby_seat = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        bool ok = true;
        static const char* const kNeedValue[] = {"--host",  "--callsign", "--aircraft", "--script",   "--rate",
                                                 "--duration", "--loss",   "--jitter",   "--log",      "--lag",
                                                 "--offset", "--serve",    "--bind",     "--start",    "--record",
                                                 "--pause-at", "--pause-for", "--mission", "--fetch",
                                                 "--out", "--world", "--world-event", "--seats", "--jet", "--hit", "--launch", "--clock",
                                                 "--password", "--lobby", "--content", "--seat"};
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
        } else if (a == "--world-join") {
            cfg.world_join = true;
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
            cfg.offset_set = true;
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
        } else if (a == "--mission" && (v = val())) {
            mission_path = v;
            serve_only = true;
        } else if (a == "--world" && (v = val())) {
            if (!load_world(v, &scfg.world)) {
                std::printf("cannot read a world from %s\n", v);
                return 1;
            }
            serve_only = true;
        } else if (a == "--world-event" && (v = val())) {
            bot::ServeConfig::WorldEv e;
            char what[16] = {}, kind = 0;
            unsigned sa = 0, sb = 0;
            if (sscanf_s(v, "%15[a-z],%c,%u,%u,%lf", what, static_cast<unsigned>(sizeof(what)), &kind, 1u, &sa, &sb, &e.at) != 5 || (kind != 'A' && kind != 'G')) {
                ok = false;
            } else {
                e.event = std::strcmp(what, "destroyed") == 0 ? 1 : std::strcmp(what, "awake") == 0 ? 2
                          : std::strcmp(what, "asleep") == 0  ? 3 : 0;
                e.kind = kind == 'A' ? 1 : 2;
                e.a = static_cast<uint16_t>(sa);
                e.b = static_cast<uint16_t>(sb);
                ok = e.event != 0;
                scfg.world_events.push_back(e);
            }
            serve_only = true;
        } else if (a == "--seats" && (v = val())) {
            ok = sscanf_s(v, "%d,%d", &scfg.seat_members, &scfg.host_member) == 2 && scfg.seat_members > 0 &&
                 scfg.host_member >= 0 && scfg.host_member < scfg.seat_members;
            serve_only = true;
        } else if (a == "--hit" && (v = val())) {
            char kind = 0;
            unsigned ha = 0, hb = 0;
            ok = sscanf_s(v, "%c,%u,%u,%lf", &kind, 1u, &ha, &hb, &scfg.hit_every) == 4 && (kind == 'A' || kind == 'G') &&
                 scfg.hit_every > 0 && ha < 0x10000 && hb < 0x10000;
            scfg.hit = ok;
            scfg.hit_kind = kind == 'A' ? 1 : 2;
            scfg.hit_a = static_cast<uint16_t>(ha);
            scfg.hit_b = static_cast<uint16_t>(hb);
            serve_only = true;
        } else if (a == "--launch" && (v = val())) {
            char name[24] = {}, kind = 0;
            unsigned guided = 0, la = 0, lb = 0;
            ok = sscanf_s(v, "%23[^,],%u,%c,%u,%u,%lf", name, static_cast<unsigned>(sizeof(name)), &guided, &kind, 1u, &la,
                          &lb, &scfg.launch_every) == 6 &&
                 (kind == 'A' || kind == 'G' || kind == 'N') && scfg.launch_every > 0 && la < 0x10000 && lb < 0x10000;
            scfg.launch = ok;
            scfg.launch_weapon = name;
            scfg.launch_guided = guided != 0;
            scfg.launch_kind = kind == 'A' ? 1 : kind == 'G' ? 2 : 0;
            scfg.launch_a = static_cast<uint16_t>(la);
            scfg.launch_b = static_cast<uint16_t>(lb);
            serve_only = true;
        } else if (a == "--seat-state") {
            scfg.seat_state = true;
            serve_only = true;
        } else if (a == "--clock" && (v = val())) {
            ok = to_double(v, &scfg.clock_start) && scfg.clock_start >= 0;
            scfg.clock = ok;
            serve_only = true;
        } else if (a == "--damage") {
            scfg.damage = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                char kind = 0;
                unsigned da = 0, db = 0;
                ok = sscanf_s(argv[++i], "%c,%u,%u", &kind, 1u, &da, &db) == 3 && (kind == 'A' || kind == 'G');
                scfg.damage_attacker_kind = kind == 'A' ? 1 : 2;
                scfg.damage_attacker_a = static_cast<uint16_t>(da);
                scfg.damage_attacker_b = static_cast<uint16_t>(db);
            }
            serve_only = true;
        } else if (a == "--stores") {
            scfg.stores = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') scfg.stores_weapon = argv[++i];
            serve_only = true;
        } else if (a == "--jet" && (v = val())) {
            ok = bot::parse_jet_script(v, &scfg.jet);
            serve_only = true;
        } else if (a == "--fetch" && (v = val())) {
            fetch_spec = v;
        } else if (a == "--out" && (v = val())) {
            fetch_out = v;
        } else if (a == "--password" && (v = val())) {
            password = v;
        } else if (a == "--lobby" && (v = val())) {
            lobby_spec = v;
        } else if (a == "--content" && (v = val())) {
            char* end = nullptr;
            lobby_content = static_cast<uint32_t>(std::strtoul(v, &end, 16));
            ok = end && end != v && !*end;
        } else if (a == "--seat" && (v = val())) {
            lobby_seat = std::atoi(v);
            ok = lobby_seat >= 1 && lobby_seat <= 255;
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
    scfg.launch_shooter_a = scfg.hit ? scfg.hit_a : 0;
    if (serve && scfg.script == bot::Script::Formation) {
        std::printf("--serve cannot fly formation: it is the lead\n");
        return 1;
    }
    if (log_path && fopen_s(&g_logfile, log_path, "w") != 0) {
        std::printf("cannot open the log file %s\n", log_path);
        return 1;
    }
    if (password.empty()) {
        char env[128] = {};
        const DWORD n = GetEnvironmentVariableA("LAZYSF2_PASSWORD", env, sizeof(env));
        if (n > 0 && n < sizeof(env)) password = env;
    }
    if (password.empty()) {
        std::printf("a session password is required: --password TEXT or LAZYSF2_PASSWORD\n");
        usage();
        return 1;
    }
    cfg.password = scfg.password = password;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    int code = 0;
    if (!lobby_spec.empty()) {
        code = run_lobby(lobby_spec, lobby_content, lobby_seat, cfg);
        if (code == 1) usage();
        if (g_logfile) std::fclose(g_logfile);
        g_logfile = nullptr;
        g_done.store(true);
        return code;
    }
    if (!fetch_spec.empty()) {
        code = run_fetch(fetch_spec, fetch_out, cfg.duration, password);
        if (code == 1) usage();
        if (g_logfile) std::fclose(g_logfile);
        g_logfile = nullptr;
        g_done.store(true);
        return code;
    }
    if (!mission_path.empty()) {
        std::vector<uint8_t> msn, side;
        if (!read_file(mission_path, &msn) || !read_file(sidecar_path(mission_path), &side) ||
            !LazyWorks::SF2Patch::net::pack_mission(std::string(side.begin(), side.end()), msn, &scfg.mission,
                                                    &scfg.mission_id, &scfg.mission_raw)) {
            std::printf("cannot read or pack %s and its sidecar\n", mission_path.c_str());
            return 1;
        }
    }
    if (serve) {
        code = bot::run_serve(scfg).code;
    } else {
        code = bot::run_bot(cfg).code;
    }
    if (g_logfile) std::fclose(g_logfile);
    g_logfile = nullptr;
    g_done.store(true);
    return code;
}
