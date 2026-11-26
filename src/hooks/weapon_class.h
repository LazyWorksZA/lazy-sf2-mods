#pragma once
// Pure logic for the weapon tracker (weapon_track.cpp), kept free of Windows headers so the tests can use it.
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The recorder's classes of weapon (the values are TacviewRec::kMissile, kBomb and kRocket).</summary>
enum class WeaponClass : uint8_t { Missile = 1, Bomb = 2, Rocket = 3 };

/// <summary>
/// The weapon data's type field (+0x298) is the ini's WeaponDataType and its guidance field (+0x398, short) is
/// GuidanceType. Type 0 is a free-fall bomb; 1 and 7 are the guided kind. Rockets are type 1 with guidance 0 (the
/// pod is a separate type 2 store, never a projectile); guided bombs have guidance 4 (laser) or 5 (TV) and stay bombs.
/// See lazy-sf2-recon: network-recon §9.2. Anything else is recorded as a missile.
/// </summary>
inline WeaponClass classify_weapon(int32_t data_type, int32_t guidance) {
    if (data_type == 0) return WeaponClass::Bomb;
    if (data_type == 1) {
        if (guidance == 0) return WeaponClass::Rocket;
        if (guidance == 4 || guidance == 5) return WeaponClass::Bomb;
    }
    return WeaponClass::Missile;
}

}  // namespace LazyWorks::SF2Patch::hooks
