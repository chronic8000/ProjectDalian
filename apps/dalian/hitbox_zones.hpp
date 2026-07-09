#pragma once

#include <string>

namespace dalian {

enum class HitboxZone { Limb, Torso, Head, Arms, Legs };

HitboxZone bone_name_to_zone(const std::string& bone_name);
float apply_hitbox_multiplier(HitboxZone zone, float base_damage);

bool hitbox_zones_self_test();

}  // namespace dalian
