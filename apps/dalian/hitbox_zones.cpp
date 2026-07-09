#include "hitbox_zones.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

namespace dalian {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

HitboxZone bone_name_to_zone(const std::string& bone_name) {
  const std::string b = lower(bone_name);
  if (contains(b, "head") || contains(b, "neck")) return HitboxZone::Head;
  if (contains(b, "spine") || contains(b, "torso") || contains(b, "pelvis") || contains(b, "hip") ||
      contains(b, "abdomen") || contains(b, "chest")) {
    return HitboxZone::Torso;
  }
  if (contains(b, "clavicle") || contains(b, "shoulder") || contains(b, "upperarm") ||
      contains(b, "forearm") || contains(b, "_l_arm") || contains(b, "_r_arm") ||
      (contains(b, "hand") && !contains(b, "thigh"))) {
    return HitboxZone::Arms;
  }
  if (contains(b, "thigh") || contains(b, "calf") || contains(b, "foot") || contains(b, "toe") ||
      contains(b, "_l_leg") || contains(b, "_r_leg")) {
    return HitboxZone::Legs;
  }
  return HitboxZone::Limb;
}

float apply_hitbox_multiplier(HitboxZone zone, float base_damage) {
  switch (zone) {
    case HitboxZone::Head:
      return base_damage * 2.5f;
    case HitboxZone::Torso:
      return base_damage * 1.0f;
    case HitboxZone::Arms:
      return base_damage * 0.75f;
    case HitboxZone::Legs:
      return base_damage * 0.5f;
    case HitboxZone::Limb:
    default:
      return base_damage * 0.65f;
  }
}

bool hitbox_zones_self_test() {
  if (bone_name_to_zone("Bip01_Head") != HitboxZone::Head) return false;
  if (bone_name_to_zone("Bip01_Spine2") != HitboxZone::Torso) return false;
  if (bone_name_to_zone("Bip01_L_UpperArm") != HitboxZone::Arms) return false;
  if (bone_name_to_zone("Bip01_L_Calf") != HitboxZone::Legs) return false;
  if (std::fabs(apply_hitbox_multiplier(HitboxZone::Head, 40.f) - 100.f) > 1e-3f) return false;
  if (std::fabs(apply_hitbox_multiplier(HitboxZone::Legs, 40.f) - 20.f) > 1e-3f) return false;
  return true;
}

}  // namespace dalian
