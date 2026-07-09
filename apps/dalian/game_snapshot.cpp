#include "game_snapshot.hpp"

#include "conquest_types.hpp"
#include "game_state.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace dalian {
namespace snapshot {

snapshot::TeamId to_snapshot_team(const dalian::TeamId team) {
  return static_cast<snapshot::TeamId>(static_cast<std::uint8_t>(team));
}

GameState build_snapshot(const dalian::GameState& sim, const std::uint32_t local_player_id,
                         const float player_yaw_deg) {
  GameState out{};

  snapshot::PlayerState ps;
  ps.id = local_player_id;
  ps.position = glm::vec3(sim.player.position.x, sim.player.position.y, sim.player.position.z);
  const float yaw = glm::radians(player_yaw_deg);
  ps.orientation = glm::angleAxis(yaw, glm::vec3(0.f, 1.f, 0.f));
  ps.health = sim.player_health;
  ps.team = snapshot::TeamId::Team1;
  ps.alive = sim.player_health > 0.f;
  ps.in_vehicle = sim.in_vehicle >= 0;
  ps.vehicle_id = sim.in_vehicle;
  out.players.push_back(ps);

  out.vehicles.reserve(sim.vehicles.size());
  for (std::size_t i = 0; i < sim.vehicles.size(); ++i) {
    const auto& v = sim.vehicles[i];
    VehicleState vs;
    vs.id = static_cast<std::uint32_t>(i + 1);
    vs.position = v.pos;
    const float hrad = glm::radians(v.heading);
    const glm::quat q_yaw = glm::angleAxis(hrad, glm::vec3(0, 1, 0));
    const glm::quat q_pitch = glm::angleAxis(glm::radians(-v.pitch), glm::vec3(1, 0, 0));
    const glm::quat q_roll = glm::angleAxis(glm::radians(-v.roll), glm::vec3(0, 0, 1));
    vs.orientation = q_yaw * q_pitch * q_roll;
    vs.health = 100.f;
    vs.is_air = v.is_air;
    vs.driver_id = -1;
    vs.gunner_id = -1;
    if (sim.in_vehicle == static_cast<int>(i)) {
      vs.driver_id = static_cast<std::int32_t>(local_player_id);
    }
    out.vehicles.push_back(vs);
  }

  out.projectiles.reserve(sim.projectiles.size());
  for (std::size_t i = 0; i < sim.projectiles.size(); ++i) {
    const auto& p = sim.projectiles[i];
    ProjectileState ps;
    ps.id = static_cast<std::uint32_t>(i + 1);
    ps.position = p.pos;
    ps.velocity = p.vel;
    ps.life = p.life;
    ps.owner_id = static_cast<std::int32_t>(local_player_id);
    out.projectiles.push_back(ps);
  }

  out.control_points.reserve(sim.control_points.size());
  for (const auto& cp : sim.control_points) {
    ControlPointState cps;
    cps.id = cp.id;
    cps.position = cp.pos;
    cps.owner = to_snapshot_team(cp.owner);
    cps.capturer = to_snapshot_team(cp.capturer);
    cps.capture_progress = cp.capture_progress;
    cps.radius = cp.radius;
    cps.capturable = cp.capturable;
    out.control_points.push_back(cps);
  }

  out.tickets = {sim.tickets.team1_tickets, sim.tickets.team2_tickets};
  out.round_time = sim.round_time;
  out.winning_team = sim.match_over ? to_snapshot_team(sim.winning_team) : snapshot::TeamId::Neutral;
  return out;
}

bool game_snapshot_self_test() {
  dalian::GameState sim;
  sim.player.position = {10.f, 5.f, -3.f};
  sim.player_health = 88.f;
  sim.tickets = {120, 95};
  sim.round_time = 42.f;

  ControlPoint cp;
  cp.id = 1;
  cp.pos = {0.f, 0.f, 0.f};
  cp.owner = dalian::TeamId::Team1;
  cp.capture_progress = 1.f;
  sim.control_points.push_back(cp);

  const GameState snap = build_snapshot(sim, 7, 90.f);
  if (snap.players.size() != 1 || snap.players[0].id != 7) return false;
  if (snap.players[0].health != 88.f) return false;
  if (snap.tickets.team1_tickets != 120 || snap.tickets.team2_tickets != 95) return false;
  if (snap.control_points.size() != 1) return false;
  if (snap.control_points[0].owner != snapshot::TeamId::Team1) return false;
  if (std::fabs(snap.round_time - 42.f) > 1e-3f) return false;
  return true;
}

}  // namespace snapshot
}  // namespace dalian
