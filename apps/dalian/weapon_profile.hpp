#pragma once

#include "engine/core/resource_manager.hpp"
#include "tweak_parser.hpp"

#include <string>

namespace dalian {

struct WeaponProfile {
  std::string name;
  float fire_rate = 0.1f;
  float damage = 34.f;
  float spread = 0.02f;
  float min_deviation = 0.02f;
  float max_deviation = 1.8f;
  float spread_per_shot = 0.08f;
  float deviation_decay = 4.5f;
  int burst_size = 1;
  float burst_shot_rate = 0.f;  // 0 = use fire_rate
  float burst_pause = 0.35f;
  int magazine_size = 30;
  int reserve_ammo = 150;
  float reload_time = 2.f;
  float tracer_speed = 900.f;
  int tracer_count = 1;
  std::string tracer_mesh = "effects/weapons/tracers/geometry/p_tracer_g/meshes/p_tracer_g.bundledmesh";
  std::string muzzle_effect = "e_muzz_mg";
  bool valid = false;
};

// Load handheld weapon tuning from a retail weapon .tweak script.
WeaponProfile load_weapon_profile(bf2::ResourceManager& resources, const std::string& tweak_vpath);

}  // namespace dalian
