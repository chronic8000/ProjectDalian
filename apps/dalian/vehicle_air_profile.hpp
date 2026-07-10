#pragma once

#include "game_sim_types.hpp"

#include <string>

namespace dalian {

// Retail Rotor / Engine / Physics / Wing blocks from vehicle .tweak / .con scripts.
struct VehicleAirProfile {
  float gear_up_speed = 45.f;
  float gear_down_speed = 35.f;
  float gear_up_height = 10.f;

  float max_thrust = 120.f;
  float sprint_factor = 1.6f;
  float sprint_limit = 100.f;
  float sprint_dissipation = 10.f;
  float sprint_recover = 30.f;
  float engine_min_rpm = 0.f;
  float engine_max_rpm = 1.f;

  float horizon_damp_angle = 45.f;
  float horizontal_damp_factor = 2.4f;
  float rotor_min_rpm = 0.f;
  float rotor_max_rpm = 1.f;
  float collective_gain = 14.f;
  float max_collective_thrust = 24.f;
  float heli_max_pitch = 38.f;
  float heli_max_roll = 32.f;
  float heli_yaw_rate = 38.f;
  float rotor_spool_up_sec = 6.f;
  float rotor_spool_down_sec = 4.f;
  float rotor_spool_collective_sec = 1.8f;
  float rotor_spin_rate = 62.f;
  float heli_drag_horiz = 0.32f;
  float heli_drag_vert = 0.55f;

  float physics_mass = 0.f;
  float physics_drag = 0.f;
  float physics_wing_lift = 0.f;
  float total_wing_lift = 0.f;
  float max_flap_lift = 11.f;

  float jet_v1 = 20.f;
  float jet_liftoff = 26.f;
  float jet_stall = 20.f;
  float jet_cruise = 45.f;
  float jet_max_ground = 52.f;
  float jet_max_air = 92.f;
  float jet_min_roll_rpm = 0.55f;
  float gravity = 9.81f;

  bool valid = false;
};

VehicleAirProfile parse_vehicle_air_profile(const std::string& tweak_text);
void apply_vehicle_air_profile(Vehicle& v, const VehicleAirProfile& p);
bool vehicle_air_profile_self_test();

}  // namespace dalian
