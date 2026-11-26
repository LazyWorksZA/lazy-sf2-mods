// ACMI Type tags for ground objects, ships and structures. Pure: no Windows or game calls.
#pragma once

#include <cstdint>
#include <string_view>

namespace LazyWorks::SF2Patch::acmi {

/// <summary>
/// The Tacview Type tags for a ground object or structure. cls is a TacviewRec class (static, vehicle, ship, structure,
/// bridge, storage). The game's category decides first (the class: vehicle, ship, bridge, storage tank); the name's
/// generic words (truck, jeep, tank, hangar and so on) only refine it. Returns a string literal, using only tags the
/// Tacview SDK defines.
/// </summary>
const char* ground_type_tags(uint8_t cls, std::string_view name);

/// <summary>True when a types-ini TargetType category names a storage or fuel tank.</summary>
bool is_storage_category(std::string_view category);

}  // namespace LazyWorks::SF2Patch::acmi
