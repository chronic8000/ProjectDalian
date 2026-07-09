#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace dalian {

enum class TeamId : std::uint8_t { Neutral = 0, Team1 = 1, Team2 = 2 };

// Runtime control point — mirrors snapshot::ControlPointState for replication.
struct ControlPoint {
  std::uint16_t id = 0;
  int bf2_cp_id = 0;
  std::string name;
  glm::vec3 pos{};
  TeamId owner = TeamId::Neutral;
  TeamId capturer = TeamId::Neutral;
  float capture_progress = 0.f;  // 0..1 toward capturer
  float radius = 15.f;
  bool capturable = true;
};

struct TicketState {
  int team1_tickets = 150;
  int team2_tickets = 150;
  float team1_bleed_accum = 0.f;  // fractional ticket drain between integer steps
  float team2_bleed_accum = 0.f;
};

inline float xz_distance_sq(const glm::vec3& a, const glm::vec3& b) {
  const float dx = a.x - b.x;
  const float dz = a.z - b.z;
  return dx * dx + dz * dz;
}

}  // namespace dalian
