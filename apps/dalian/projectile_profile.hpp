#pragma once

#include "engine/core/resource_manager.hpp"

#include <string>

namespace dalian {

struct ProjectileProfile {
  std::string template_name;
  float damage = 100.f;
  float min_damage = 0.2f;
  float acceleration = 300.f;
  float max_speed = 125.f;
  float launch_velocity = 55.f;
  float explosion_radius = 6.f;
  float explosion_damage = 100.f;
  float gravity = 9.81f;
  float gravity_modifier = 1.f;
  float mass = 20.f;
  float drag = 1.f;
  float life = 12.f;
  bool guided = false;
  float turn_rate = 2.8f;
  bool valid = false;
};

ProjectileProfile parse_projectile_profile(const std::string& tweak_text,
                                           const std::string& template_name = {});
ProjectileProfile load_projectile_profile(bf2::ResourceManager& resources,
                                          const std::string& template_name);
bool projectile_profile_self_test();

}  // namespace dalian
