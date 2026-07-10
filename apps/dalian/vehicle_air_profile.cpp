#include "vehicle_air_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace dalian {
namespace {

enum class Block { None, Vehicle, Rotor, Engine, Physics, Wing };

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

bool is_air_vehicle_type(const std::string& t) {
  return t == "vehicle" || t == "airvehicle" || t == "helicopter" || t == "fixedwingvehicle" ||
         t == "airplane";
}

}  // namespace

VehicleAirProfile parse_vehicle_air_profile(const std::string& tweak_text) {
  VehicleAirProfile p;
  Block block = Block::None;
  std::istringstream in(tweak_text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string c = lower(cmd);
    if (c == "objecttemplate.create" || c == "objecttemplate.activesafe") {
      std::string typ, name;
      ls >> typ >> name;
      const std::string t = lower(typ);
      if (t == "rotor") block = Block::Rotor;
      else if (t == "engine") block = Block::Engine;
      else if (t == "physics") block = Block::Physics;
      else if (t == "wing") block = Block::Wing;
      else if (is_air_vehicle_type(t)) block = Block::Vehicle;
      else block = Block::None;
      if (t == "wing") p.valid = true;
      continue;
    }
    std::string val;
    if (!(ls >> val)) continue;
    const float fv = parse_float(val);

    auto setf = [&](const char* key, float& field) {
      if (c == key) {
        field = fv;
        p.valid = true;
      }
    };

    setf("objecttemplate.setgearupspeed", p.gear_up_speed);
    setf("objecttemplate.setgeardownspeed", p.gear_down_speed);
    setf("objecttemplate.setgearupheight", p.gear_up_height);
    setf("objecttemplate.horizondampangle", p.horizon_damp_angle);
    setf("objecttemplate.horizontaldampanglefactor", p.horizontal_damp_factor);
    setf("objecttemplate.setcollectivethrust", p.collective_gain);
    setf("objecttemplate.maxcollectivethrust", p.max_collective_thrust);
    setf("objecttemplate.setmaxpitch", p.heli_max_pitch);
    setf("objecttemplate.setmaxroll", p.heli_max_roll);
    setf("objecttemplate.setyawrate", p.heli_yaw_rate);
    setf("objecttemplate.setspooluptime", p.rotor_spool_up_sec);
    setf("objecttemplate.setspooldowntime", p.rotor_spool_down_sec);
    setf("objecttemplate.setliftoffspeed", p.jet_liftoff);
    setf("objecttemplate.setstallspeed", p.jet_stall);
    setf("objecttemplate.setcruisespeed", p.jet_cruise);
    setf("objecttemplate.setmaxspeed", p.jet_max_air);
    setf("objecttemplate.setgravity", p.gravity);
    setf("objecttemplate.physics.mass", p.physics_mass);
    setf("objecttemplate.mass", p.physics_mass);
    setf("objecttemplate.physics.drag", p.physics_drag);
    setf("objecttemplate.drag", p.physics_drag);
    setf("objecttemplate.physics.winglift", p.physics_wing_lift);
    setf("objecttemplate.setwinglift", p.total_wing_lift);
    setf("objecttemplate.setflaplift", p.max_flap_lift);

    if (c == "objecttemplate.maxthrust" || c == "objecttemplate.engine.maxthrust") {
      p.max_thrust = fv;
      p.valid = true;
    } else if (c == "objecttemplate.sprintfactor" || c == "objecttemplate.engine.sprintfactor") {
      p.sprint_factor = fv;
      p.valid = true;
    } else if (c == "objecttemplate.sprintlimit" || c == "objecttemplate.engine.sprintlimit") {
      p.sprint_limit = fv;
      p.valid = true;
    } else if (c == "objecttemplate.sprintdissipation" ||
               c == "objecttemplate.engine.sprintdissipation") {
      p.sprint_dissipation = fv;
      p.valid = true;
    } else if (c == "objecttemplate.sprintrecover" || c == "objecttemplate.engine.sprintrecover") {
      p.sprint_recover = fv;
      p.valid = true;
    } else if (c == "objecttemplate.setminrpm") {
      if (block == Block::Engine) p.engine_min_rpm = fv;
      else p.rotor_min_rpm = fv;
      p.valid = true;
    } else if (c == "objecttemplate.setmaxrpm") {
      if (block == Block::Engine) p.engine_max_rpm = fv;
      else p.rotor_max_rpm = fv;
      p.valid = true;
    } else if (c == "objecttemplate.physicstype") {
      p.valid = true;
    } else if (c == "objecttemplate.setwinglift") {
      p.total_wing_lift = std::max(p.total_wing_lift, fv);
      p.valid = true;
    } else if (c == "objecttemplate.setflaplift") {
      p.max_flap_lift = std::max(p.max_flap_lift, fv);
      p.valid = true;
    }
  }

  if (p.collective_gain <= 0.f && p.max_thrust > 0.f) {
    p.collective_gain = p.max_thrust / 8.57f;
  }
  if (p.total_wing_lift <= 0.f && p.physics_wing_lift > 0.f) {
    p.total_wing_lift = p.physics_wing_lift;
  }
  return p;
}

void apply_vehicle_air_profile(Vehicle& v, const VehicleAirProfile& p) {
  if (!p.valid) return;
  v.max_thrust = std::max(40.f, p.max_thrust);
  v.sprint_factor = std::max(1.f, p.sprint_factor);
  v.gear_up_height = std::max(2.f, p.gear_up_height);
  v.sprint_dissipation = std::max(1.f, p.sprint_dissipation);
  v.sprint_recover = std::max(1.f, p.sprint_recover);
  v.sprint_limit = std::clamp(p.sprint_limit > 1.5f ? p.sprint_limit / 100.f : p.sprint_limit, 0.1f,
                             1.f);

  v.horizon_damp_angle = p.horizon_damp_angle;
  v.horizontal_damp_factor = p.horizontal_damp_factor;
  v.collective_gain = std::max(4.f, p.collective_gain);
  v.max_collective_thrust = std::max(8.f, p.max_collective_thrust);
  v.heli_max_pitch = p.heli_max_pitch;
  v.heli_max_roll = p.heli_max_roll;
  v.heli_yaw_rate = p.heli_yaw_rate;
  v.rotor_spool_up = std::max(0.5f, p.rotor_spool_up_sec);
  v.rotor_spool_down = std::max(0.5f, p.rotor_spool_down_sec);
  v.rotor_spool_collective = std::max(0.5f, p.rotor_spool_collective_sec);
  v.rotor_spin_rate = std::max(10.f, p.rotor_spin_rate);
  // Arcade linear drag (1/s). Keep low so helis carry momentum for J-hooks;
  // angular damp (horizon spring) stays aggressive separately.
  v.heli_drag_horiz = std::clamp(p.heli_drag_horiz, 0.12f, 0.55f);
  v.heli_drag_vert = std::clamp(p.heli_drag_vert, 0.2f, 0.9f);
  v.gravity = std::max(1.f, p.gravity);

  v.jet_v1 = std::max(8.f, p.jet_v1);
  v.jet_liftoff = std::max(v.jet_v1 + 2.f, p.jet_liftoff);
  v.jet_stall = std::max(8.f, p.jet_stall);
  v.jet_cruise = std::max(20.f, p.jet_cruise);
  v.jet_max_ground = std::max(30.f, p.jet_max_ground > 20.f ? p.jet_max_ground : p.jet_cruise * 1.15f);
  v.jet_max_air = std::max(v.jet_max_ground, p.jet_max_air);
  v.jet_min_roll_rpm = std::clamp(p.jet_min_roll_rpm > 0.01f ? p.jet_min_roll_rpm : 0.55f, 0.2f,
                                  0.9f);
  v.physics_drag = p.physics_drag;
  v.physics_mass = p.physics_mass;
  v.physics_wing_lift = p.physics_wing_lift;

  if (!v.is_heli) {
    v.gear_anim_up_rate = std::max(0.15f, p.gear_up_speed / 90.f);
    v.gear_anim_down_rate = std::max(0.15f, p.gear_down_speed / 90.f);
    if (p.total_wing_lift > 0.f) {
      v.wing_body_lift = p.total_wing_lift * 0.41f;
      v.wing_flap_lift = p.max_flap_lift;
    } else if (p.physics_wing_lift > 0.f) {
      v.wing_body_lift = p.physics_wing_lift * 0.41f;
    }
  }
}

bool vehicle_air_profile_self_test() {
  static const char* kSample = R"(
ObjectTemplate.create Vehicle
ObjectTemplate.activeSafe Vehicle
ObjectTemplate.mass 6500
ObjectTemplate.drag 0.05
ObjectTemplate.setGearUpSpeed 50
ObjectTemplate.setGearDownSpeed 40
ObjectTemplate.setGearUpHeight 12
ObjectTemplate.create Engine
ObjectTemplate.activeSafe Engine
ObjectTemplate.maxThrust 145
ObjectTemplate.sprintFactor 1.8
ObjectTemplate.sprintLimit 100
ObjectTemplate.sprintDissipation 8
ObjectTemplate.sprintRecover 25
ObjectTemplate.create Rotor
ObjectTemplate.activeSafe Rotor
ObjectTemplate.HorizonDampAngle 50
ObjectTemplate.HorizontalDampAngleFactor 2.8
ObjectTemplate.setMaxRPM 1
ObjectTemplate.create Wing
ObjectTemplate.setWingLift 0.72
ObjectTemplate.setFlapLift 13
)";

  const VehicleAirProfile p = parse_vehicle_air_profile(kSample);
  if (!p.valid) return false;
  if (std::fabs(p.max_thrust - 145.f) > 1e-3f) return false;
  if (std::fabs(p.physics_mass - 6500.f) > 1e-3f) return false;
  if (std::fabs(p.horizon_damp_angle - 50.f) > 1e-3f) return false;
  if (std::fabs(p.gear_up_height - 12.f) > 1e-3f) return false;
  if (std::fabs(p.total_wing_lift - 0.72f) > 1e-3f) return false;
  if (std::fabs(p.sprint_dissipation - 8.f) > 1e-3f) return false;

  Vehicle v;
  v.is_heli = false;
  apply_vehicle_air_profile(v, p);
  if (std::fabs(v.max_thrust - 145.f) > 1e-3f) return false;
  if (std::fabs(v.gear_up_height - 12.f) > 1e-3f) return false;
  if (v.wing_body_lift <= 0.2f) return false;
  return true;
}

}  // namespace dalian
