#include "vehicle_weapon_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_map>

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

struct WeaponBlock {
  std::string name;
  float fire_rpm = 0.f;
  float damage = 0.f;
  float min_dev = 0.f;
  float max_dev = 0.f;
  std::string projectile_template;
  bool has_minigun_muzz = false;
};

struct ProjectileBlock {
  std::string name;
  ProjectileProfile profile;
};

VehicleWeaponRole classify_weapon(const std::string& name, bool has_minigun_muzz) {
  const std::string n = lower(name);
  if (n.find("flare") != std::string::npos) return VehicleWeaponRole::Unknown;
  if (n.find("smoke") != std::string::npos) return VehicleWeaponRole::Unknown;
  if (n.find("coax") != std::string::npos) return VehicleWeaponRole::CoaxGun;
  if (n.find("hellfire") != std::string::npos || n.find("tvmissile") != std::string::npos)
    return VehicleWeaponRole::GunnerRockets;
  if (n.find("hydra") != std::string::npos || n.find("rocketlauncher") != std::string::npos ||
      n.find("rocketpod") != std::string::npos) {
    if (n.find("cogunner") != std::string::npos || n.find("gunner") != std::string::npos)
      return VehicleWeaponRole::GunnerRockets;
    return VehicleWeaponRole::PilotRockets;
  }
  if (has_minigun_muzz || n.find("gunturret") != std::string::npos ||
      (n.find("gun") != std::string::npos && n.find("launcher") == std::string::npos))
    return VehicleWeaponRole::GunnerGun;
  if (n.find("barrel") != std::string::npos &&
      (n.find("ustnk") != std::string::npos || n.find("tank") != std::string::npos ||
       n.find("plz") != std::string::npos || n.find("ztz") != std::string::npos))
    return VehicleWeaponRole::MainCannon;
  if (n.find("hmg") != std::string::npos || n.find("cupola") != std::string::npos)
    return VehicleWeaponRole::CoaxGun;
  return VehicleWeaponRole::Unknown;
}

void apply_slot(VehicleWeaponLoadout& out, const WeaponBlock& wb,
                const std::unordered_map<std::string, ProjectileProfile>& projectiles) {
  const VehicleWeaponRole role = classify_weapon(wb.name, wb.has_minigun_muzz);
  if (role == VehicleWeaponRole::Unknown) return;

  VehicleWeaponSlot slot;
  slot.role = role;
  slot.name = wb.name;
  slot.fire_rate = wb.fire_rpm > 1.f ? 60.f / wb.fire_rpm : 0.1f;
  slot.damage = wb.damage > 0.f ? wb.damage : 20.f;
  slot.min_deviation = wb.min_dev > 0.f ? wb.min_dev * 0.001f : 0.02f;
  slot.max_deviation = wb.max_dev > 0.f ? wb.max_dev * 0.001f : 1.f;
  slot.projectile_template = wb.projectile_template;
  if (!wb.projectile_template.empty()) {
    const auto it = projectiles.find(lower(wb.projectile_template));
    if (it != projectiles.end()) slot.projectile = it->second;
  }
  if (role == VehicleWeaponRole::PilotRockets || role == VehicleWeaponRole::GunnerRockets) {
    if (slot.fire_rate > 0.2f) slot.fire_rate = 0.28f;
    if (!slot.projectile.valid && slot.projectile_template.empty()) {
      slot.projectile = ProjectileProfile{};
      slot.projectile.damage = 200.f;
      slot.projectile.acceleration = 190.f;
      slot.projectile.max_speed = 240.f;
      slot.projectile.launch_velocity = 60.f;
      slot.projectile.explosion_radius = 6.f;
      slot.projectile.explosion_damage = 150.f;
      slot.projectile.life = 6.f;
      slot.projectile.valid = true;
    }
    slot.range = 320.f;
  } else if (role == VehicleWeaponRole::GunnerGun) {
    if (slot.fire_rate > 0.2f) slot.fire_rate = 0.09f;
    slot.range = 260.f;
  } else if (role == VehicleWeaponRole::MainCannon) {
    if (slot.fire_rate > 1.f) slot.fire_rate = 4.f;
    slot.range = 450.f;
  } else if (role == VehicleWeaponRole::CoaxGun) {
    if (slot.fire_rate > 0.2f) slot.fire_rate = 0.12f;
    slot.range = 200.f;
  }
  slot.valid = true;

  auto better = [&](const VehicleWeaponSlot& cur) {
    return !cur.valid || slot.damage > cur.damage;
  };
  switch (role) {
    case VehicleWeaponRole::PilotRockets:
      if (better(out.pilot_rockets)) out.pilot_rockets = slot;
      break;
    case VehicleWeaponRole::GunnerGun:
      if (better(out.gunner_gun)) out.gunner_gun = slot;
      break;
    case VehicleWeaponRole::GunnerRockets:
      if (better(out.gunner_rockets)) out.gunner_rockets = slot;
      break;
    case VehicleWeaponRole::MainCannon:
      if (better(out.main_gun)) out.main_gun = slot;
      break;
    case VehicleWeaponRole::CoaxGun:
      if (better(out.coax_gun)) out.coax_gun = slot;
      break;
    default:
      break;
  }
}

}  // namespace

VehicleWeaponLoadout parse_vehicle_weapons(const std::string& vehicle_tweak_text) {
  VehicleWeaponLoadout out;
  std::unordered_map<std::string, ProjectileProfile> projectiles;
  WeaponBlock cur_weapon;
  ProjectileBlock cur_proj;
  enum class Mode { None, Weapon, Projectile } mode = Mode::None;

  auto flush_weapon = [&]() {
    if (mode == Mode::Weapon && !cur_weapon.name.empty()) {
      apply_slot(out, cur_weapon, projectiles);
    }
    cur_weapon = {};
    mode = Mode::None;
  };
  auto flush_proj = [&]() {
    if (mode == Mode::Projectile && !cur_proj.name.empty() && cur_proj.profile.valid) {
      projectiles[lower(cur_proj.name)] = cur_proj.profile;
    }
    cur_proj = {};
    mode = Mode::None;
  };

  std::istringstream in(vehicle_tweak_text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd, a, b;
    ls >> cmd >> a >> b;
    const std::string c = lower(cmd);
    if (c == "objecttemplate.create") {
      flush_weapon();
      flush_proj();
      const std::string t = lower(a);
      if (t == "genericfirearm") {
        mode = Mode::Weapon;
        cur_weapon.name = b;
      } else if (t == "genericprojectile") {
        mode = Mode::Projectile;
        cur_proj.name = b;
        cur_proj.profile.template_name = b;
        cur_proj.profile.valid = true;
      } else {
        mode = Mode::None;
      }
      continue;
    }
    std::string val;
    if (!(ls >> val)) val = a;
    if (mode == Mode::Weapon) {
      if (c == "objecttemplate.fire.roundsperminute") cur_weapon.fire_rpm = parse_float(val);
      else if (c == "objecttemplate.damage") cur_weapon.damage = parse_float(val);
      else if (c == "objecttemplate.deviation.mindev") cur_weapon.min_dev = parse_float(val);
      else if (c == "objecttemplate.deviation.maxdev") cur_weapon.max_dev = parse_float(val);
      else if (c == "objecttemplate.projectiletemplate") cur_weapon.projectile_template = val;
      else if (c == "objecttemplate.addtemplate" && lower(val).find("e_muzz_minigun") != std::string::npos)
        cur_weapon.has_minigun_muzz = true;
    } else if (mode == Mode::Projectile) {
      ProjectileProfile& p = cur_proj.profile;
      if (c == "objecttemplate.damage") p.damage = parse_float(val);
      else if (c == "objecttemplate.mindamage") p.min_damage = parse_float(val);
      else if (c == "objecttemplate.acceleration") p.acceleration = parse_float(val);
      else if (c == "objecttemplate.maxspeed") p.max_speed = parse_float(val);
      else if (c == "objecttemplate.velocity") p.launch_velocity = parse_float(val);
      else if (c == "objecttemplate.detonation.explosionradius") p.explosion_radius = parse_float(val);
      else if (c == "objecttemplate.detonation.explosiondamage") p.explosion_damage = parse_float(val);
      else if (c == "objecttemplate.createcomponent" && lower(a) == "seekclosesttargetcomp") p.guided = true;
      else if (c == "objecttemplate.createcomponent" && lower(a) == "defaultfollowcomp") p.guided = true;
      else if (c == "objecttemplate.follow.maxyaw") p.turn_rate = parse_float(val) * 0.017453292f;
      else if (c == "objecttemplate.timetolive") {
        const auto slash = val.find('/');
        if (slash != std::string::npos) p.life = parse_float(val.substr(slash + 1));
      }
    }
  }
  flush_weapon();
  flush_proj();
  return out;
}

void resolve_vehicle_weapon_projectiles(bf2::ResourceManager& resources,
                                        const std::string& vehicle_tweak_text,
                                        VehicleWeaponLoadout& loadout) {
  auto resolve_slot = [&](VehicleWeaponSlot& slot) {
    if (!slot.valid) return;
    if (!slot.projectile.valid && !slot.projectile_template.empty()) {
      ProjectileProfile embedded =
          parse_projectile_profile(vehicle_tweak_text, slot.projectile_template);
      if (embedded.valid) slot.projectile = embedded;
    }
    if (!slot.projectile.valid && !slot.projectile_template.empty()) {
      slot.projectile = load_projectile_profile(resources, slot.projectile_template);
    }
    if (slot.projectile.valid) {
      if (slot.damage <= 0.f) slot.damage = slot.projectile.damage;
    }
  };
  resolve_slot(loadout.pilot_rockets);
  resolve_slot(loadout.gunner_gun);
  resolve_slot(loadout.gunner_rockets);
  resolve_slot(loadout.main_gun);
  resolve_slot(loadout.coax_gun);
}

bool vehicle_weapon_profile_self_test() {
  static const char* kSample = R"(
ObjectTemplate.create GenericFireArm AHE_AH1Z_HydraLauncher
ObjectTemplate.fire.roundsPerMinute 420
ObjectTemplate.projectileTemplate ahe_ah1z_HydraLauncher_Projectile
ObjectTemplate.create GenericProjectile ahe_ah1z_HydraLauncher_Projectile
ObjectTemplate.damage 333
ObjectTemplate.acceleration 300
ObjectTemplate.maxSpeed 125
ObjectTemplate.detonation.explosionRadius 6
ObjectTemplate.detonation.explosionDamage 200
ObjectTemplate.create GenericFireArm AHE_AH1Z_Gun
ObjectTemplate.fire.roundsPerMinute 600
ObjectTemplate.addTemplate e_muzz_minigun
ObjectTemplate.projectileTemplate AHE_AH1Z_Gun_Projectile
ObjectTemplate.damage 20
ObjectTemplate.deviation.minDev 0.5
)";
  const VehicleWeaponLoadout w = parse_vehicle_weapons(kSample);
  return w.pilot_rockets.valid && w.gunner_gun.valid && w.pilot_rockets.projectile.max_speed > 100.f;
}

}  // namespace dalian
