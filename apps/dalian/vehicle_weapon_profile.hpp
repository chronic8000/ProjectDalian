#pragma once

#include "projectile_profile.hpp"

#include "engine/core/resource_manager.hpp"

#include <string>

namespace dalian {

enum class VehicleWeaponRole {
  Unknown,
  PilotRockets,
  GunnerGun,
  GunnerRockets,
  MainCannon,
  CoaxGun,
};

struct VehicleWeaponSlot {
  VehicleWeaponRole role = VehicleWeaponRole::Unknown;
  std::string name;
  float fire_rate = 0.1f;
  float damage = 20.f;
  float min_deviation = 0.02f;
  float max_deviation = 1.f;
  float range = 260.f;
  std::string projectile_template;
  ProjectileProfile projectile;
  bool valid = false;
};

struct VehicleWeaponLoadout {
  VehicleWeaponSlot pilot_rockets;
  VehicleWeaponSlot gunner_gun;
  VehicleWeaponSlot gunner_rockets;
  VehicleWeaponSlot main_gun;
  VehicleWeaponSlot coax_gun;
};

VehicleWeaponLoadout parse_vehicle_weapons(const std::string& vehicle_tweak_text);
void resolve_vehicle_weapon_projectiles(bf2::ResourceManager& resources,
                                        const std::string& vehicle_tweak_text,
                                        VehicleWeaponLoadout& loadout);
bool vehicle_weapon_profile_self_test();

}  // namespace dalian
