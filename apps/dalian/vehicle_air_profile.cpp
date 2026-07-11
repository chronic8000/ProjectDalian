#include "vehicle_air_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace dalian {
namespace {

enum class Block { None, Vehicle, Rotor, Engine, Physics, Wing, Spring };

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
  // Per-wing scratch while inside a Wing block (setInputToPitch + PositionOffset).
  bool wing_input_pitch = false;
  float wing_ox = 0.f, wing_oy = 0.f, wing_oz = 0.f;
  float elev_z_acc = 0.f;
  int elev_n = 0;
  float ail_x_acc = 0.f;
  int ail_n = 0;

  std::istringstream in(tweak_text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string c = lower(cmd);
    if (c == "objecttemplate.create" || c == "objecttemplate.activesafe") {
      // Flush previous wing sample into elevator/aileron arms.
      if (block == Block::Wing) {
        if (wing_input_pitch && wing_oz < -0.5f) {
          elev_z_acc += wing_oz;
          ++elev_n;
        } else if (std::fabs(wing_ox) > 1.0f) {
          ail_x_acc += std::fabs(wing_ox);
          ++ail_n;
        } else if (wing_oz < -1.5f) {
          elev_z_acc += wing_oz;
          ++elev_n;
        }
      }
      wing_input_pitch = false;
      wing_ox = wing_oy = wing_oz = 0.f;

      std::string typ, name;
      ls >> typ >> name;
      const std::string t = lower(typ);
      if (t == "rotor")
        block = Block::Rotor;
      else if (t == "engine")
        block = Block::Engine;
      else if (t == "physics")
        block = Block::Physics;
      else if (t == "wing")
        block = Block::Wing;
      else if (t == "spring")
        block = Block::Spring;
      else if (is_air_vehicle_type(t))
        block = Block::Vehicle;
      else
        block = Block::None;
      if (t == "wing") p.valid = true;
      continue;
    }

    // DragModifier x/y/z (BF2 often writes 1/0/1 as one token).
    if (c == "objecttemplate.dragmodifier" || c == "objecttemplate.physics.dragmodifier") {
      std::string rest;
      std::getline(ls, rest);
      for (char& ch : rest) {
        if (ch == '/' || ch == ',' || ch == '\t') ch = ' ';
      }
      std::istringstream rs(rest);
      float x = 1.f, y = 1.f, z = 1.f;
      if (rs >> x) {
        if (!(rs >> y)) y = x;
        if (!(rs >> z)) z = x;
      }
      p.drag_modifier = glm::vec3(x > 0.f ? x : 1.f, y > 0.f ? y : 1.f, z > 0.f ? z : 1.f);
      p.valid = true;
      continue;
    }

    // PositionOffset x/y/z — wing leverage for torque (§7.1).
    if (c == "objecttemplate.setpositionoffset" || c == "objecttemplate.positionoffset") {
      std::string rest;
      std::getline(ls, rest);
      for (char& ch : rest) {
        if (ch == '/' || ch == ',' || ch == '\t') ch = ' ';
      }
      std::istringstream rs(rest);
      float x = 0.f, y = 0.f, z = 0.f;
      if (rs >> x) {
        rs >> y;
        rs >> z;
      }
      if (block == Block::Wing) {
        wing_ox = x;
        wing_oy = y;
        wing_oz = z;
        p.valid = true;
      }
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

    if (c == "objecttemplate.setinputtopitch" && block == Block::Wing) {
      wing_input_pitch = fv > 0.5f || val == "1" || lower(val) == "true";
      p.valid = true;
      continue;
    }

    setf("objecttemplate.setgearupspeed", p.gear_up_speed);
    setf("objecttemplate.setgeardownspeed", p.gear_down_speed);
    setf("objecttemplate.setgearupheight", p.gear_up_height);
    setf("objecttemplate.horizondampangle", p.horizon_damp_angle);
    setf("objecttemplate.horizontaldampanglefactor", p.horizontal_damp_factor);
    setf("objecttemplate.damphorizontalvelfactor", p.damp_horizontal_vel);
    setf("objecttemplate.decreaseangletozero", p.decrease_aoa_to_zero);
    setf("objecttemplate.defaultangleofattack", p.default_aoa);
    setf("objecttemplate.maxangleofattack", p.max_aoa);
    setf("objecttemplate.attackspeed", p.attack_speed);
    setf("objecttemplate.airfloweffect", p.airflow_yaw_factor);
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
    setf("objecttemplate.gravitymodifier", p.gravity_modifier);
    setf("objecttemplate.inertiamodifier", p.inertia_modifier);
    setf("objecttemplate.physics.mass", p.physics_mass);
    setf("objecttemplate.mass", p.physics_mass);
    setf("objecttemplate.physics.drag", p.physics_drag);
    setf("objecttemplate.drag", p.physics_drag);
    setf("objecttemplate.physics.winglift", p.physics_wing_lift);
    setf("objecttemplate.setregulatetolift", p.regulate_to_lift);
    setf("objecttemplate.setwingtoregulatorratio", p.wing_to_regulator);
    setf("objecttemplate.setautomaticreset", p.automatic_reset);
    // Landing gear springs only (§7.2) — ignore setStrength on unrelated nodes.
    if (block == Block::Spring) {
      setf("objecttemplate.setstrength", p.gear_spring);
      setf("objecttemplate.setdamping", p.gear_damping);
    }
    setf("objecttemplate.spring.setstrength", p.gear_spring);
    setf("objecttemplate.spring.setdamping", p.gear_damping);

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
      if (block == Block::Engine)
        p.engine_min_rpm = fv;
      else
        p.rotor_min_rpm = fv;
      p.valid = true;
    } else if (c == "objecttemplate.setmaxrpm") {
      if (block == Block::Engine)
        p.engine_max_rpm = fv;
      else
        p.rotor_max_rpm = fv;
      p.valid = true;
    } else if (c == "objecttemplate.setwinglift") {
      p.total_wing_lift = std::max(p.total_wing_lift, fv);
      p.valid = true;
    } else if (c == "objecttemplate.setflaplift") {
      p.max_flap_lift = std::max(p.max_flap_lift, fv);
      p.valid = true;
    } else if (c == "objecttemplate.physicstype") {
      p.valid = true;
    }
  }

  // Flush last wing.
  if (block == Block::Wing) {
    if (wing_input_pitch && wing_oz < -0.5f) {
      elev_z_acc += wing_oz;
      ++elev_n;
    } else if (std::fabs(wing_ox) > 1.0f) {
      ail_x_acc += std::fabs(wing_ox);
      ++ail_n;
    } else if (wing_oz < -1.5f) {
      elev_z_acc += wing_oz;
      ++elev_n;
    }
  }
  if (elev_n > 0) p.elevator_z_offset = elev_z_acc / static_cast<float>(elev_n);
  if (ail_n > 0) p.aileron_x_offset = ail_x_acc / static_cast<float>(ail_n);

  if (p.collective_gain <= 0.f && p.max_thrust > 0.f) {
    p.collective_gain = p.max_thrust / 8.57f;
  }
  if (p.total_wing_lift <= 0.f && p.physics_wing_lift > 0.f) {
    p.total_wing_lift = p.physics_wing_lift;
  }
  if (p.max_aoa < 4.f) p.max_aoa = 16.f;
  if (p.attack_speed < 0.2f) p.attack_speed = 2.8f;
  return p;
}

void apply_vehicle_air_profile(Vehicle& v, const VehicleAirProfile& p) {
  if (!p.valid) return;
  v.max_thrust = std::max(40.f, p.max_thrust);
  v.sprint_factor = std::max(1.f, p.sprint_factor);
  v.gear_up_height = std::max(2.f, p.gear_up_height);
  v.sprint_dissipation = std::max(1.f, p.sprint_dissipation);
  v.sprint_recover = std::max(1.f, p.sprint_recover);
  v.sprint_limit =
      std::clamp(p.sprint_limit > 1.5f ? p.sprint_limit / 100.f : p.sprint_limit, 0.1f, 1.f);

  v.horizon_damp_angle = std::max(8.f, p.horizon_damp_angle);
  v.horizontal_damp_factor = std::max(0.2f, p.horizontal_damp_factor);
  v.damp_horizontal_vel = std::clamp(p.damp_horizontal_vel, 0.2f, 4.f);
  v.decrease_aoa_to_zero = std::clamp(p.decrease_aoa_to_zero, 0.3f, 6.f);
  v.default_aoa = std::clamp(p.default_aoa, 0.f, 12.f);
  v.max_aoa = std::clamp(p.max_aoa, 6.f, 28.f);
  v.attack_speed = std::clamp(p.attack_speed, 0.5f, 8.f);
  v.airflow_yaw_factor = std::clamp(p.airflow_yaw_factor, 0.15f, 1.f);

  v.collective_gain = std::max(4.f, p.collective_gain);
  v.max_collective_thrust = std::max(8.f, p.max_collective_thrust);
  v.heli_max_pitch = p.heli_max_pitch;
  v.heli_max_roll = p.heli_max_roll;
  v.heli_yaw_rate = p.heli_yaw_rate;
  v.rotor_spool_up = std::max(0.5f, p.rotor_spool_up_sec);
  v.rotor_spool_down = std::max(0.5f, p.rotor_spool_down_sec);
  v.rotor_spool_collective = std::max(0.5f, p.rotor_spool_collective_sec);
  v.rotor_spin_rate = std::max(10.f, p.rotor_spin_rate);
  v.heli_drag_horiz = std::clamp(p.heli_drag_horiz, 0.12f, 0.55f);
  v.heli_drag_vert = std::clamp(p.heli_drag_vert, 0.2f, 0.9f);

  v.gravity = std::max(1.f, p.gravity);
  v.gravity_modifier = std::clamp(p.gravity_modifier > 0.05f ? p.gravity_modifier : 1.f, 0.35f, 1.6f);
  v.inertia_modifier =
      std::clamp(p.inertia_modifier > 0.05f ? p.inertia_modifier : 1.f, 0.45f, 2.2f);
  v.drag_modifier = p.drag_modifier;

  v.jet_v1 = std::max(8.f, p.jet_v1);
  v.jet_liftoff = std::max(v.jet_v1 + 2.f, p.jet_liftoff);
  v.jet_stall = std::max(8.f, p.jet_stall);
  v.jet_cruise = std::max(20.f, p.jet_cruise);
  v.jet_max_ground =
      std::max(30.f, p.jet_max_ground > 20.f ? p.jet_max_ground : p.jet_cruise * 1.15f);
  v.jet_max_air = std::max(v.jet_max_ground, p.jet_max_air);
  v.jet_min_roll_rpm =
      std::clamp(p.jet_min_roll_rpm > 0.01f ? p.jet_min_roll_rpm : 0.55f, 0.2f, 0.9f);
  v.physics_drag = p.physics_drag;
  v.physics_mass = p.physics_mass;
  v.physics_wing_lift = p.physics_wing_lift;
  v.regulate_to_lift = std::max(0.4f, p.regulate_to_lift);
  v.wing_to_regulator = std::max(0.2f, p.wing_to_regulator);
  v.automatic_reset = std::clamp(p.automatic_reset, 0.f, 1.f);
  v.jet_pitch_rate = std::clamp(p.jet_pitch_rate, 1.5f, 8.f);
  v.jet_roll_rate = std::clamp(p.jet_roll_rate, 1.5f, 8.f);
  v.jet_bank_turn_gain = std::clamp(p.jet_bank_turn_gain, 20.f, 90.f);
  v.elevator_z_offset = p.elevator_z_offset < -0.5f ? p.elevator_z_offset : -5.5f;
  v.aileron_x_offset = p.aileron_x_offset > 0.5f ? p.aileron_x_offset : 3.2f;
  v.gear_spring = std::clamp(p.gear_spring > 1.f ? p.gear_spring : 55.f, 20.f, 120.f);
  v.gear_damping = std::clamp(p.gear_damping > 1.f ? p.gear_damping : 14.f, 4.f, 40.f);

  if (!v.is_heli) {
    v.gear_anim_up_rate = std::max(0.15f, p.gear_up_speed / 90.f);
    v.gear_anim_down_rate = std::max(0.15f, p.gear_down_speed / 90.f);
    // Retail wingLift is a small scalar; map into our arcade body-lift range.
    if (p.total_wing_lift > 0.f) {
      v.wing_body_lift = std::clamp(p.total_wing_lift * 0.55f, 0.35f, 1.35f);
      v.wing_flap_lift = std::clamp(p.max_flap_lift, 4.f, 22.f);
    } else if (p.physics_wing_lift > 0.f) {
      v.wing_body_lift = std::clamp(p.physics_wing_lift * 0.55f, 0.35f, 1.35f);
    }
  }
}

bool vehicle_air_profile_self_test() {
  static const char* kSample = R"(
ObjectTemplate.create Vehicle
ObjectTemplate.activeSafe Vehicle
ObjectTemplate.mass 6500
ObjectTemplate.drag 0.05
ObjectTemplate.DragModifier 1.0 0.6 1.0
ObjectTemplate.GravityModifier 0.85
ObjectTemplate.InertiaModifier 1.1
ObjectTemplate.setGearUpSpeed 50
ObjectTemplate.setGearDownSpeed 40
ObjectTemplate.setGearUpHeight 12
ObjectTemplate.setRegulateToLift 1.2
ObjectTemplate.setAutomaticReset 1
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
ObjectTemplate.DampHorizontalVelFactor 1.5
ObjectTemplate.DefaultAngleOfAttack 3
ObjectTemplate.MaxAngleOfAttack 18
ObjectTemplate.AttackSpeed 3.0
ObjectTemplate.DecreaseAngleToZero 2.2
ObjectTemplate.setMaxRPM 1
ObjectTemplate.create Wing
ObjectTemplate.setWingLift 0.72
ObjectTemplate.setFlapLift 13
ObjectTemplate.setInputToPitch 1
ObjectTemplate.setPositionOffset 0 -0.2 -6.5
ObjectTemplate.create Wing
ObjectTemplate.setPositionOffset 4.0 0 0.5
ObjectTemplate.setWingLift 0.4
ObjectTemplate.create Spring
ObjectTemplate.setStrength 70
ObjectTemplate.setDamping 18
)";

  const VehicleAirProfile p = parse_vehicle_air_profile(kSample);
  if (!p.valid) return false;
  if (std::fabs(p.max_thrust - 145.f) > 1e-3f) return false;
  if (std::fabs(p.physics_mass - 6500.f) > 1e-3f) return false;
  if (std::fabs(p.horizon_damp_angle - 50.f) > 1e-3f) return false;
  if (std::fabs(p.gear_up_height - 12.f) > 1e-3f) return false;
  if (std::fabs(p.total_wing_lift - 0.72f) > 1e-3f) return false;
  if (std::fabs(p.sprint_dissipation - 8.f) > 1e-3f) return false;
  if (std::fabs(p.default_aoa - 3.f) > 1e-3f) return false;
  if (std::fabs(p.max_aoa - 18.f) > 1e-3f) return false;
  if (std::fabs(p.damp_horizontal_vel - 1.5f) > 1e-3f) return false;
  if (std::fabs(p.gravity_modifier - 0.85f) > 1e-3f) return false;
  if (std::fabs(p.drag_modifier.y - 0.6f) > 1e-3f) return false;
  if (p.elevator_z_offset > -5.f) return false;
  if (p.aileron_x_offset < 3.5f) return false;
  if (std::fabs(p.gear_spring - 70.f) > 1e-3f) return false;
  if (std::fabs(p.gear_damping - 18.f) > 1e-3f) return false;

  Vehicle v;
  v.is_heli = false;
  apply_vehicle_air_profile(v, p);
  if (std::fabs(v.max_thrust - 145.f) > 1e-3f) return false;
  if (std::fabs(v.gear_up_height - 12.f) > 1e-3f) return false;
  if (v.wing_body_lift <= 0.2f) return false;
  if (std::fabs(v.gravity_modifier - 0.85f) > 1e-3f) return false;
  if (v.elevator_z_offset > -5.f) return false;
  return true;
}

}  // namespace dalian
