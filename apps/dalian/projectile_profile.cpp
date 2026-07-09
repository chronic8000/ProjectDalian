#include "projectile_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace dalian {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

float parse_float(const std::string& s) {
  try {
    return std::stof(s);
  } catch (...) {
    return 0.f;
  }
}

std::vector<std::string> projectile_tweak_candidates(const std::string& template_name) {
  const std::string n = lower(template_name);
  std::vector<std::string> paths;
  auto add = [&](const std::string& p) {
    if (std::find(paths.begin(), paths.end(), p) == paths.end()) paths.push_back(p);
  };
  add("Weapons/Armament/missiles/" + n + "/" + n + ".tweak");
  if (n.find("hydra") != std::string::npos) add("Weapons/Armament/missiles/hydra_70/hydra_70.tweak");
  if (n.find("hellfire") != std::string::npos || n.find("agm114") != std::string::npos)
    add("Weapons/Armament/missiles/agm114_hellfire/agm114_hellfire.tweak");
  if (n.find("igla") != std::string::npos || n.find("9k38") != std::string::npos)
    add("Weapons/Armament/missiles/igla_9k38/igla_9k38.tweak");
  if (n.find("predator") != std::string::npos || n.find("at_") != std::string::npos)
    add("Weapons/Armament/missiles/at_predator/at_predator.tweak");
  if (n.find("rpg") != std::string::npos || n.find("insgr") != std::string::npos)
    add("Weapons/Armament/missiles/insgr_rpg/insgr_rpg.tweak");
  if (n.find("eryx") != std::string::npos)
    add("Weapons/Armament/missiles/eryx/eryx.tweak");
  return paths;
}

}  // namespace

ProjectileProfile parse_projectile_profile(const std::string& tweak_text,
                                           const std::string& template_name) {
  ProjectileProfile p;
  p.template_name = template_name;
  enum class Block { None, Projectile, Weapon };
  Block block = Block::None;
  std::string block_name;
  bool has_seek = false;
  bool has_follow = false;
  float follow_yaw = 0.f;

  std::istringstream in(tweak_text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd, a, b;
    ls >> cmd >> a >> b;
    const std::string c = lower(cmd);

    if (c == "objecttemplate.create") {
      const std::string t = lower(a);
      if (t == "genericprojectile") {
        block = Block::Projectile;
        block_name = b;
        if (!template_name.empty() && lower(block_name) != lower(template_name)) {
          block = Block::None;
        } else {
          p.valid = true;
        }
      } else if (t == "genericfirearm") {
        block = Block::Weapon;
        block_name = b;
      } else {
        block = Block::None;
      }
      continue;
    }
    if (block == Block::None) continue;

    std::string val;
  if (!(ls >> val)) val = a;

    auto setf = [&](const char* key, float& field) {
      if (c == key) field = parse_float(val);
    };

    setf("objecttemplate.damage", p.damage);
    setf("objecttemplate.mindamage", p.min_damage);
    setf("objecttemplate.acceleration", p.acceleration);
    setf("objecttemplate.maxspeed", p.max_speed);
    setf("objecttemplate.velocity", p.launch_velocity);
    setf("objecttemplate.mass", p.mass);
    setf("objecttemplate.drag", p.drag);
    setf("objecttemplate.gravitymodifier", p.gravity_modifier);
    setf("objecttemplate.detonation.explosionradius", p.explosion_radius);
    setf("objecttemplate.detonation.explosiondamage", p.explosion_damage);
    setf("objecttemplate.follow.maxyaw", follow_yaw);
    if (c == "objecttemplate.timetolive") {
      const auto slash = val.find('/');
      if (slash != std::string::npos) p.life = parse_float(val.substr(slash + 1));
    }
    if (c == "objecttemplate.projectiletemplate" && block == Block::Weapon) {
      p.template_name = val;
    }
    if (c == "objecttemplate.createcomponent" && lower(a) == "seekclosesttargetcomp") has_seek = true;
    if (c == "objecttemplate.createcomponent" && lower(a) == "defaultfollowcomp") has_follow = true;
  }

  p.guided = has_seek || has_follow;
  if (follow_yaw > 1.f) p.turn_rate = follow_yaw * 0.017453292f;
  if (p.launch_velocity <= 1.f && p.max_speed > 1.f) p.launch_velocity = std::min(60.f, p.max_speed * 0.45f);
  if (p.explosion_radius <= 0.f) p.explosion_radius = 6.f;
  if (p.explosion_damage <= 0.f) p.explosion_damage = p.damage;
  return p;
}

ProjectileProfile load_projectile_profile(bf2::ResourceManager& resources,
                                          const std::string& template_name) {
  ProjectileProfile best;
  best.template_name = template_name;
  for (const auto& path : projectile_tweak_candidates(template_name)) {
    if (const auto bytes = resources.read_bytes(path)) {
      const ProjectileProfile p = parse_projectile_profile(
          std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()), template_name);
      if (p.valid) return p;
    }
  }
  if (lower(template_name).find("predator") != std::string::npos) {
    best.damage = 500.f;
    best.acceleration = 175.f;
    best.max_speed = 45.f;
    best.launch_velocity = 30.f;
    best.explosion_radius = 2.f;
    best.explosion_damage = 125.f;
    best.guided = false;
    best.life = 8.f;
    best.valid = true;
  } else if (lower(template_name).find("igla") != std::string::npos) {
    best.damage = 300.f;
    best.acceleration = 300.f;
    best.max_speed = 225.f;
    best.launch_velocity = 20.f;
    best.explosion_radius = 4.f;
    best.explosion_damage = 75.f;
    best.guided = true;
    best.turn_rate = 2.5f;
    best.life = 12.f;
    best.valid = true;
  } else if (lower(template_name).find("rpg") != std::string::npos ||
             lower(template_name).find("insgr") != std::string::npos) {
    best.damage = 400.f;
    best.acceleration = 175.f;
    best.max_speed = 45.f;
    best.launch_velocity = 28.f;
    best.explosion_radius = 3.f;
    best.explosion_damage = 200.f;
    best.guided = false;
    best.gravity_modifier = 1.f;
    best.life = 6.f;
    best.valid = true;
  } else if (lower(template_name).find("hydra") != std::string::npos) {
    best.damage = 350.f;
    best.acceleration = 300.f;
    best.max_speed = 125.f;
    best.launch_velocity = 60.f;
    best.explosion_radius = 6.f;
    best.explosion_damage = 100.f;
    best.valid = true;
  }
  return best;
}

bool projectile_profile_self_test() {
  static const char* kSample = R"(
ObjectTemplate.create GenericProjectile hydra_70
ObjectTemplate.damage 350
ObjectTemplate.acceleration 300
ObjectTemplate.maxSpeed 125
ObjectTemplate.detonation.explosionRadius 6
ObjectTemplate.detonation.explosionDamage 100
)";
  const ProjectileProfile p = parse_projectile_profile(kSample, "hydra_70");
  return p.valid && std::fabs(p.max_speed - 125.f) < 1e-3f && std::fabs(p.explosion_radius - 6.f) < 1e-3f;
}

}  // namespace dalian
