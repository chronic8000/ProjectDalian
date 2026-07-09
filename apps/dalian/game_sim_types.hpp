#pragma once

#include "vehicle_weapon_profile.hpp"
#include "conquest_types.hpp"

#include "engine/formats/effects/effect_bundle.hpp"
#include "engine/physics/missile.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace dalian {

// Per-frame input gathered from SDL (no renderer / audio knowledge).
struct PlayerInput {
  glm::vec3 move_wish{0.f};  // world-space walk direction (XZ), may be zero
  glm::vec3 look_forward{0.f, 0.f, -1.f};
  glm::vec3 look_right{1.f, 0.f, 0.f};
  glm::vec3 eye{0.f};
  float yaw_delta = 0.f;
  float pitch_delta = 0.f;
  bool sprint = false;
  bool jump = false;
  bool fire = false;
  bool fire_secondary = false;
  bool boost = false;
  bool throttle_up = false;
  bool throttle_down = false;
  bool yaw_left = false;
  bool yaw_right = false;
  bool invert_air = false;
  bool deploy_open = false;
  bool mouse_look = true;
  bool drone_mode = false;
  bool reloading_blocked = false;
  bool launch_missile = false;
  bool launch_at = false;
  bool flare_request = false;
  bool gear_toggle = false;
  bool pitch_up = false;  // Space — continuous pitch-up (BF2 jet dogfight bind)
  int seat_switch = -1;  // 0-7 for F1-F8, else -1
  bool enter_exit = false;
  float air_pitch_stick = 0.f;
  float air_roll_stick = 0.f;
  bool air_stick_moved = false;
};

struct HitCapsule {
  glm::vec3 a{};
  glm::vec3 b{};
  float r = 0.1f;
  int zone = 0;  // 0 limb, 1 torso, 2 head
};

struct Tracer {
  glm::vec3 a{};
  glm::vec3 b{};
  float life = 0.f;
};

struct Impact {
  glm::vec3 p{};
  float life = 0.f;
};

struct Projectile {
  glm::vec3 pos{};
  glm::vec3 vel{};
  float life = 0.f;
};

struct ActiveMissile {
  bf2::MissileController m{};
  int homing_enemy = -1;
  glm::vec3 prev_pos{};
  float smoke_timer = 0.f;
  float explosion_radius = 9.f;
  float explosion_damage = 150.f;
};

struct Smoke {
  glm::vec3 p{};
  glm::vec3 vel{};
  float age = 0.f;
  float life = 1.6f;
  float size = 0.45f;
  float birth_size = 0.45f;
  std::uint8_t kind = 0;
  glm::vec3 tint{0.f};
  bool use_graphs = false;
  bf2::Graph4 transp_graph;
  bf2::Graph4 size_graph;
};

struct Explosion {
  glm::vec3 p{};
  float age = 0.f;
  float life = 0.85f;
  float scale = 1.f;
};

struct Flare {
  glm::vec3 p{};
  glm::vec3 v{};
  float life = 0.f;
};

struct VehiclePart {
  std::string mesh_key;
  glm::mat4 local{1.f};
};

struct VehicleWheelSlot {
  int geom_part = -1;
  std::string mesh_key;
  glm::mat4 rest{1.f};
  bool steers = false;
  float gear_tuck_angle = 65.f;  // degrees when landing gear stows (jets)
  int gear_tuck_axis = 1;        // 0=Y 1=X 2=Z in render euler order
};

struct VehicleGearSlot {
  int geom_part = -1;
  std::string mesh_key;
  glm::mat4 rest{1.f};
  float gear_tuck_angle = 90.f;
  int gear_tuck_axis = 2;
};

struct Vehicle {
  std::string mesh_key;
  glm::mat4 model{1.f};
  glm::vec3 origin{};
  std::vector<VehiclePart> parts;
  glm::vec3 pos{};
  float heading = 0.f;
  float speed = 0.f;
  bool is_air = false;
  float rotor_spin = 0.f;
  float rotor_rpm = 0.f;
  float pitch = 0.f;
  float roll = 0.f;
  glm::vec3 vel{0.f};
  bool has_gunner_seat = false;
  bool is_heli = false;
  bool is_boat = false;
  float throttle = 0.f;
  bool wheels_on_ground = true;
  float jet_rpm = 0.f;       // engine spool 0..1 (lags throttle on jets)
  bool jet_airborne = false; // stable flag for camera / ground-vs-air logic
  bool jet_gear_down = true;
  float jet_gear_anim = 0.f;
  float gear_anim_up_rate = 0.55f;
  float gear_anim_down_rate = 0.7f;
  float wing_body_lift = 0.62f;
  float wing_flap_lift = 11.f;
  float max_thrust = 120.f;     // Engine.maxThrust
  float sprint_factor = 1.6f;   // Engine.sprintFactor
  float sprint_limit = 1.f;
  float sprint_dissipation = 10.f;
  float sprint_recover = 30.f;
  float gear_up_height = 10.f;
  float physics_drag = 0.f;
  float physics_mass = 0.f;
  float physics_wing_lift = 0.f;
  float horizon_damp_angle = 45.f;
  float horizontal_damp_factor = 2.4f;
  float collective_gain = 14.f;
  float max_collective_thrust = 24.f;
  float heli_max_pitch = 38.f;
  float heli_max_roll = 32.f;
  float heli_yaw_rate = 38.f;
  float rotor_spool_up = 6.f;
  float rotor_spool_down = 4.f;
  float rotor_spool_collective = 1.8f;
  float rotor_spin_rate = 62.f;
  float heli_drag_horiz = 0.9f;
  float heli_drag_vert = 1.4f;
  float gravity = 9.81f;
  float jet_v1 = 20.f;
  float jet_liftoff = 26.f;
  float jet_stall = 20.f;
  float jet_cruise = 45.f;
  float jet_max_ground = 52.f;
  float jet_max_air = 92.f;
  float jet_min_roll_rpm = 0.55f;
  float jet_sprint = 1.f;
  glm::vec3 jet_exhaust_l{0.44f, 0.f, -5.9f};  // model-space engine nozzles
  glm::vec3 jet_exhaust_r{-0.44f, 0.f, -5.9f};
  struct SeatSlot {
    const char* name = "SEAT";
    int occupant = -1;
  };
  std::vector<SeatSlot> seats;
  glm::vec3 ground_normal{0.f, 1.f, 0.f};
  float clearance = 0.f;
  float land_clearance = 0.f;
  glm::vec3 col_half{1.6f, 1.4f, 3.0f};
  std::vector<VehicleWheelSlot> wheels;
  std::vector<VehicleGearSlot> gear_parts;
  std::vector<float> wheel_spin;
  float visual_steer = 0.f;
  VehicleWeaponLoadout weapons{};
};

struct Enemy {
  glm::vec3 pos{};
  glm::vec3 home{};
  TeamId team = TeamId::Team2;
  float yaw = 0.f;
  float health = 100.f;
  float fire_rate = 0.12f;
  float damage = 28.f;
  float spread = 0.04f;
  float anim_time = 0.f;
  float alert = 0.f;
  float burst_cooldown = 0.f;
  int burst_left = 0;
  float shot_timer = 0.f;
  float hit_flash = 0.f;
  float death_time = 0.f;
  bool alive = true;
  bool moving = false;
  glm::vec3 patrol_target{};
  float patrol_wait = 0.f;
  std::vector<HitCapsule> caps;
};

}  // namespace dalian
