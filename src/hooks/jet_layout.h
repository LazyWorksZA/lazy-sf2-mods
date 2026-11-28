#pragma once
// AircraftObject fields and system classes used by the jet census and the jet mirror (Phase 5). July 2013 build
// only (stamp and size below). See lazy-sf2-recon: network-recon §11.1 to §11.4.
#include <cstddef>
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks::jet {

inline constexpr uint32_t kAoStamp = 0x51d220bd, kAoSize = 0xc2000;

// The aircraft object.
inline constexpr size_t kFlags = 0x168, kDamage = 0x16c, kStateWord = 0x5f0, kRequestWord = 0x5f4, kInputs = 0x5f8;
inline constexpr size_t kComponents = 0x32c;  // list: +0x04 count, +0x10 array
inline constexpr size_t kEngineCount = 0x668, kEngines = 0x670, kMaxEngines = 16;  // short count, pointers
inline constexpr size_t kFuelFlow = 0x6b8, kFuelTotal = 0x6bc, kActiveTank = 0x6c4;
inline constexpr size_t kLoadout = 0x6cc, kLoadoutStations = 32, kLoadoutEntry = 12;  // id +0, count +8, state +0xa
inline constexpr size_t kSelectedStation = 0x8d0, kGunMask = 0x8e8, kChaffTotal = 0x8f0, kFlareTotal = 0x8f2;
inline constexpr size_t kRemoteFire = 0x918;
inline constexpr size_t kInputCount = 16;  // AIRCRAFTCONTROLLIST.INI channels the census prints
inline constexpr size_t kInputThrottle = 3, kInputThrottleLast = 7, kInputGun1 = 12, kInputGun2 = 13, kInputFire = 14;
// AIRCRAFTCONTROLLIST.INI: PITCH, ROLL, YAW (control surfaces, nosewheel steering) and their trims.
inline constexpr size_t kInputAxes[6] = {0, 1, 2, 9, 10, 11};

// A list (ObjectComponentClass and the aircraft): +0x04 count, +0x10 array of pointers.
inline constexpr size_t kListCount = 0x04, kListArray = 0x10;
// A component.
inline constexpr size_t kCompAttached = 0x08, kCompIndex = 0x0c, kCompStrength = 0x10, kCompDamage = 0x14,
                        kCompShowsDamage = 0x1c, kCompSystems = 0x50;
// Every system (ObjectSystemClass).
inline constexpr size_t kSysWorking = 0x04, kSysStrength = 0x10, kSysDamage = 0x14;
// AeroEngineClass: running (short), throttle, AB stage (short), on fire, nozzle.
inline constexpr size_t kEngRunning = 0x2c, kEngThrottle = 0x30, kEngFire = 0x40, kEngNozzle = 0x7c, kEngAb = 0x88;
inline constexpr size_t kEngSetRunningSlot = 22;
inline constexpr uint32_t kRvaEngSetRunning = 0x388e0;  // thiscall(short), writes +0x2c
// AircraftFuelTankClass, AircraftFixedGunClass, AircraftDecoyDispenserClass.
inline constexpr size_t kTankFuel = 0x44, kTankFire = 0x3c, kTankExternal = 0x56;
inline constexpr size_t kGunBits = 0x38, kGunBurst = 0x3c, kGunRounds = 0x3e, kGunReloads = 0x60;
inline constexpr size_t kDispChaff = 0x30, kDispFlares = 0x32, kDispChaffDue = 0x38, kDispFlaresDue = 0x3a;
// AircraftWeaponStationClass: data (+0x78 station id), its loadout entry, target id, full count.
inline constexpr size_t kStaData = 0x2c, kStaEntry = 0x38, kStaTarget = 0x7c, kStaFull = 0x80;

/// <summary>System classes by vtable RVA (the factory's types, §11.1); 0 kinds are not told apart.</summary>
enum class SysKind : uint8_t { Unknown, JetEngine, FuelTank, FixedGun, Station, Dispenser, Animation };
inline constexpr struct {
    uint32_t vtable;
    SysKind kind;
    const char* name;
} kSysVtables[] = {
    {0x93494, SysKind::JetEngine, "jet engine"},   {0x93abc, SysKind::FuelTank, "fuel tank"},
    {0x94bec, SysKind::FixedGun, "fixed gun"},     {0x94cdc, SysKind::Station, "weapon station"},
    {0x9335c, SysKind::Dispenser, "dispenser"},    {0x878bc, SysKind::Animation, "animation"},
};
inline SysKind sys_kind(uint32_t vtable_rva, const char** name) {
    for (const auto& v : kSysVtables)
        if (v.vtable == vtable_rva) {
            if (name) *name = v.name;
            return v.kind;
        }
    if (name) *name = "?";
    return SysKind::Unknown;
}

}  // namespace LazyWorks::SF2Patch::hooks::jet
