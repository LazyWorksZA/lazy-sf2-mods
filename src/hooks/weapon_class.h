#pragma once
// Pure logic for the weapon tracker (weapon_track.cpp), kept free of Windows headers so the tests can use it.
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The recorder's classes of weapon (the values are TacviewRec::kMissile, kBomb and kRocket).</summary>
enum class WeaponClass : uint8_t { Missile = 1, Bomb = 2, Rocket = 3 };

/// <summary>
/// The weapon data's type field (the object's data pointer at +0x14c, then +0x298) is the ini's WeaponDataType:
/// 0 is a free-fall bomb (the loader reads its retarded, fin and spin stabilised and lift/drag settings), 1 and 7 are
/// guided missiles (7 also allows decoys), 2 is a rocket pod. The game's missile object only sets itself up for 0, 1
/// and 7; any other type that shows up is recorded as a missile.
/// </summary>
inline WeaponClass classify_weapon_type(int32_t data_type) {
    switch (data_type) {
        case 0:
            return WeaponClass::Bomb;
        case 2:
            return WeaponClass::Rocket;
        default:
            return WeaponClass::Missile;
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
