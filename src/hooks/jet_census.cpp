// Systems census (developer tool, Phase 5 task 1): a text dump of one aircraft's engines, stores, fuel, guns,
// countermeasures, components and systems, for the probe's Ctrl+Shift+D snapshot. Reads only through
// ReadProcessMemory, so a stale pointer gives a "?" instead of a fault. See lazy-sf2-recon: network-recon §11.1.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "hooks/jet_layout.h"

namespace LazyWorks::SF2Patch::hooks {

std::string jet_census(const void* object, uintptr_t ao);

namespace {

template <typename T>
bool rd(uintptr_t at, T* v) {
    SIZE_T got = 0;
    return at >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), v, sizeof(T), &got) &&
           got == sizeof(T);
}

void put(std::string& s, const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(line, _TRUNCATE, fmt, ap);
    va_end(ap);
    s += line;
}

constexpr int kMaxSystemLines = 256;

uint32_t rva_of(uint32_t p, uintptr_t ao) {
    return ao && p >= ao && p < ao + jet::kAoSize ? static_cast<uint32_t>(p - ao) : 0;
}

void system_line(std::string& s, uint32_t sys, uintptr_t ao, int index) {
    using namespace jet;
    uint32_t vt = 0;
    int32_t working = 0;
    float strength = 0, damage = 0;
    if (!rd(sys, &vt)) {
        put(s, "      sys %d %08x: not readable\n", index, sys);
        return;
    }
    const uint32_t vr = rva_of(vt, ao);
    const char* name = "?";
    const SysKind k = sys_kind(vr, &name);
    rd(sys + kSysWorking, &working);
    rd(sys + kSysStrength, &strength);
    rd(sys + kSysDamage, &damage);
    put(s, "      sys %d %08x vt +0x%05x (%s) working %d strength %.1f damage %.1f", index, sys, vr, name, working,
        strength, damage);
    int16_t a = 0, b = 0, c = 0, d = 0;
    float f = 0, g = 0;
    int32_t i = 0;
    switch (k) {
        case SysKind::JetEngine:
            rd(sys + kEngRunning, &a);
            rd(sys + kEngThrottle, &f);
            rd(sys + kEngAb, &b);
            rd(sys + kEngFire, &i);
            rd(sys + kEngNozzle, &g);
            put(s, " | running %d throttle %.3f ab %d fire %d nozzle %.2f", a, f, b, i, g);
            break;
        case SysKind::FuelTank:
            rd(sys + kTankFuel, &f);
            rd(sys + kTankFire, &i);
            rd(sys + kTankExternal, &a);
            put(s, " | fuel %.1f kg fire %d external %d", f, i, a);
            break;
        case SysKind::FixedGun: {
            uint8_t bits = 0;
            rd(sys + kGunBits, &bits);
            rd(sys + kGunBurst, &a);
            rd(sys + kGunRounds, &b);
            rd(sys + kGunReloads, &i);
            put(s, " | bits 0x%02x burst %d rounds %d reloads %d", bits, a, b, i);
            break;
        }
        case SysKind::Dispenser:
            rd(sys + kDispChaff, &a);
            rd(sys + kDispFlares, &b);
            rd(sys + kDispChaffDue, &c);
            rd(sys + kDispFlaresDue, &d);
            put(s, " | chaff %d flares %d due %d/%d", a, b, c, d);
            break;
        case SysKind::Station: {
            uint32_t data = 0, entry = 0;
            int32_t target = 0, id = -1;
            rd(sys + kStaData, &data);
            rd(sys + kStaEntry, &entry);
            rd(sys + kStaTarget, &target);
            rd(sys + kStaFull, &i);
            if (data) rd(data + 0x78, &id);
            put(s, " | station id %d entry %08x target %08x full %d", id, entry, target, i);
            break;
        }
        default:
            break;
    }
    s += "\n";
}

}  // namespace

std::string jet_census(const void* object, uintptr_t ao) {
    using namespace jet;
    const uintptr_t o = reinterpret_cast<uintptr_t>(object);
    std::string s;
    uint32_t flags = 0, state = 0, request = 0, inputs = 0;
    int32_t damage = 0, remote_fire = 0;
    rd(o + kFlags, &flags);
    rd(o + kDamage, &damage);
    rd(o + kStateWord, &state);
    rd(o + kRequestWord, &request);
    rd(o + kInputs, &inputs);
    rd(o + kRemoteFire, &remote_fire);
    put(s, "aircraft %08x: flags 0x%08x damage %d state 0x%08x request 0x%08x remote fire 0x%x\n",
        static_cast<unsigned>(o), flags, damage, state, request, remote_fire);
    s += "  input:";
    for (size_t i = 0; i < kInputCount; ++i) {
        float v = 0;
        if (rd(inputs + 4 * i, &v)) put(s, " [%zu] %.3f", i, v);
        else s += " ?";
    }
    s += "\n";
    int16_t engines = 0;
    rd(o + kEngineCount, &engines);
    put(s, "  engines %d:", engines);
    for (int i = 0; i < engines && i < static_cast<int>(kMaxEngines); ++i) {
        uint32_t e = 0, vt = 0, setter = 0;
        int16_t run = 0, ab = 0;
        float thr = 0;
        if (!rd(o + kEngines + 4 * i, &e) || !rd(e, &vt)) continue;
        rd(vt + 4 * kEngSetRunningSlot, &setter);
        rd(e + kEngRunning, &run);
        rd(e + kEngThrottle, &thr);
        rd(e + kEngAb, &ab);
        put(s, " [%d] vt +0x%05x setter %s running %d throttle %.3f ab %d", i, rva_of(vt, ao),
            rva_of(setter, ao) == kRvaEngSetRunning ? "ok" : "OTHER", run, thr, ab);
    }
    float flow = 0, total = 0;
    int16_t tank = 0, station = 0, chaff = 0, flares = 0;
    uint32_t gun_mask = 0;
    rd(o + kFuelFlow, &flow);
    rd(o + kFuelTotal, &total);
    rd(o + kActiveTank, &tank);
    rd(o + kSelectedStation, &station);
    rd(o + kGunMask, &gun_mask);
    rd(o + kChaffTotal, &chaff);
    rd(o + kFlareTotal, &flares);
    put(s, "\n  fuel %.1f kg flow %.3f active tank %d; selected station %d gun mask 0x%x chaff %d flares %d\n", total,
        flow, tank, station, gun_mask, chaff, flares);
    s += "  loadout:";
    for (size_t i = 0; i < kLoadoutStations; ++i) {
        int32_t id = -1;
        int16_t count = 0, st = 0;
        const uintptr_t at = o + kLoadout + kLoadoutEntry * i;
        if (!rd(at, &id) || id < 0) continue;
        rd(at + 8, &count);
        rd(at + 10, &st);
        put(s, " [%zu] id %d x%d state %d", i, id, count, st);
    }
    s += "\n";
    int32_t ncomp = 0;
    uint32_t comps = 0;
    rd(o + kComponents + kListCount, &ncomp);
    rd(o + kComponents + kListArray, &comps);
    put(s, "  components %d:\n", ncomp);
    int lines = 0;  // bounds the reads made under the aircraft table's lock
    for (int i = 0; i < ncomp && i < 64 && lines < kMaxSystemLines; ++i) {
        uint32_t c = 0, vt = 0, systems = 0;
        int32_t attached = 0, index = 0, shows = 0, nsys = 0;
        float strength = 0, dmg = 0;
        if (!rd(comps + 4 * i, &c) || !c || !rd(c, &vt)) continue;
        rd(c + kCompAttached, &attached);
        rd(c + kCompIndex, &index);
        rd(c + kCompStrength, &strength);
        rd(c + kCompDamage, &dmg);
        rd(c + kCompShowsDamage, &shows);
        rd(c + kCompSystems + kListCount, &nsys);
        rd(c + kCompSystems + kListArray, &systems);
        put(s, "    comp %d %08x vt +0x%05x attached %d index %d strength %.1f damage %.1f shows %d systems %d\n", i, c,
            rva_of(vt, ao), attached, index, strength, dmg, shows, nsys);
        for (int j = 0; j < nsys && j < 64 && lines < kMaxSystemLines; ++j, ++lines) {
            uint32_t sys = 0;
            if (rd(systems + 4 * j, &sys) && sys) system_line(s, sys, ao, j);
        }
    }
    return s;
}

}  // namespace LazyWorks::SF2Patch::hooks
