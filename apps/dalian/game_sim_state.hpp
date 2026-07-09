#pragma once

// Net-ready Plain Old Data snapshot of a BF2 match at one simulation tick.
// No gameplay logic, SDL, or OpenGL — safe for headless server replication.
// Distinct from runtime `dalian::GameState` in game_state.hpp (the live sim bag).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

namespace dalian {
namespace snapshot {

enum class TeamId : std::uint8_t { Neutral = 0, Team1 = 1, Team2 = 2 };

// Per-tick input from one client (keyboard/mouse → data, no platform headers).
struct PlayerInput {
  glm::vec3 move_wish{0.f};       // normalized XZ walk direction in world space
  glm::vec3 look_forward{0.f, 0.f, -1.f};
  glm::vec3 look_right{1.f, 0.f, 0.f};
  glm::vec3 eye{0.f};
  float yaw_delta = 0.f;
  float pitch_delta = 0.f;
  float air_pitch_stick = 0.f;
  float air_roll_stick = 0.f;
  bool sprint = false;
  bool jump = false;
  bool fire = false;
  bool fire_secondary = false;
  bool boost = false;
  bool throttle_up = false;
  bool throttle_down = false;
  bool yaw_left = false;
  bool yaw_right = false;
  bool reload = false;
  bool enter_exit_vehicle = false;
  bool deploy_request = false;
  bool mouse_look = true;
  std::int8_t seat_switch = -1;  // 0..7 for F1-F8, else -1
};

struct PlayerState {
  std::uint32_t id = 0;
  glm::vec3 position{};
  glm::quat orientation{1.f, 0.f, 0.f, 0.f};
  float health = 100.f;
  TeamId team = TeamId::Neutral;
  std::uint8_t kit = 0;
  std::uint8_t weapon = 0;
  bool alive = true;
  bool in_vehicle = false;
  std::int32_t vehicle_id = -1;
};

struct VehicleState {
  std::uint32_t id = 0;
  glm::vec3 position{};
  glm::quat orientation{1.f, 0.f, 0.f, 0.f};
  float health = 100.f;
  float heat = 0.f;  // engine / weapon overheat 0..1
  std::int32_t driver_id = -1;
  std::int32_t gunner_id = -1;
  bool is_air = false;
};

struct ProjectileState {
  std::uint32_t id = 0;
  glm::vec3 position{};
  glm::vec3 velocity{};
  float damage = 0.f;
  std::int32_t owner_id = -1;
  float life = 0.f;
};

struct ControlPointState {
  std::uint16_t id = 0;
  glm::vec3 position{};
  TeamId owner = TeamId::Neutral;
  TeamId capturer = TeamId::Neutral;
  float capture_progress = 0.f;  // 0..1 toward capturer
  float radius = 15.f;
  bool capturable = true;
};

struct TicketState {
  int team1_tickets = 150;
  int team2_tickets = 150;
};

// Authoritative match snapshot at a single tick (server → clients).
struct GameState {
  std::vector<PlayerState> players;
  std::vector<VehicleState> vehicles;
  std::vector<ProjectileState> projectiles;
  std::vector<ControlPointState> control_points;
  TicketState tickets;
  float round_time = 0.f;
  TeamId winning_team = TeamId::Neutral;
};

}  // namespace snapshot
}  // namespace dalian
