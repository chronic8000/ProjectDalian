#pragma once

#include "game_sim_types.hpp"

#include <glm/glm.hpp>

#include <string>

namespace dalian {

// Retail Rotor / Engine / Physics / Wing blocks from vehicle .tweak / .con scripts.
// Mirrors Refractor 2 ObjectTemplate keys used for BF2 air feel (see flight research).
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

  // Helicopter PID / rotor AoA (c_ETHelicopter-style).
  float horizon_damp_angle = 45.f;
  float horizontal_damp_factor = 2.4f;
  float damp_horizontal_vel = 1.35f;     // DampHorizontalVelFactor
  float decrease_aoa_to_zero = 2.0f;     // DecreaseAngleToZero rate
  float default_aoa = 2.5f;              // DefaultAngleOfAttack (hover trim, deg)
  float max_aoa = 16.f;                  // MaxAngleOfAttack (deg)
  float attack_speed = 2.8f;             // AttackSpeed — how fast rotor tilts
  float airflow_yaw_factor = 0.65f;      // high-speed yaw authority loss
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

  // Newtonian overrides.
  float physics_mass = 0.f;
  float physics_drag = 0.f;
  float physics_wing_lift = 0.f;
  float gravity = 9.81f;
  float gravity_modifier = 1.f;          // GravityModifier
  float inertia_modifier = 1.f;          // InertiaModifier (scales angular response)
  glm::vec3 drag_modifier{1.f, 1.f, 1.f}; // DragModifier x/y/z

  // Fixed-wing.
  float total_wing_lift = 0.f;           // setWingLift (accumulated)
  float max_flap_lift = 11.f;            // setFlapLift
  float regulate_to_lift = 1.15f;        // setRegulateToLift
  float wing_to_regulator = 1.f;         // setWingToRegulatorRatio
  float automatic_reset = 1.f;           // setAutomaticReset → stick recenter strength
  float jet_v1 = 20.f;
  float jet_liftoff = 26.f;
  float jet_stall = 20.f;
  float jet_cruise = 45.f;
  float jet_max_ground = 52.f;
  float jet_max_air = 92.f;
  float jet_min_roll_rpm = 0.55f;
  float jet_pitch_rate = 4.2f;
  float jet_roll_rate = 3.6f;
  float jet_bank_turn_gain = 52.f;
  // Wing PositionOffset leverage (elevator Z aft, aileron X span) — §7.1 torque.
  float elevator_z_offset = -5.5f;
  float aileron_x_offset = 3.2f;
  float gear_spring = 55.f;   // landing spring strength
  float gear_damping = 14.f;  // landing damper

  bool valid = false;
};

VehicleAirProfile parse_vehicle_air_profile(const std::string& tweak_text);
void apply_vehicle_air_profile(Vehicle& v, const VehicleAirProfile& p);
bool vehicle_air_profile_self_test();

}  // namespace dalian
