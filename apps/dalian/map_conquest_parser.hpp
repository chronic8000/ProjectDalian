#pragma once

#include "conquest_types.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace dalian {

struct ParsedControlPoint {
  std::string name;
  std::string label;
  int bf2_id = 0;
  TeamId initial_team = TeamId::Neutral;
  float radius = 15.f;
  glm::vec3 pos{};
};

struct ParsedSoldierSpawn {
  glm::vec3 pos{};
  int bf2_cp_id = 0;
};

struct MapConquestLayout {
  std::vector<ParsedControlPoint> control_points;
  std::vector<ParsedSoldierSpawn> soldier_spawns;
  int team1_faction_id = 0;   // United States (USMC/US Army)
  int team2_faction_id = 3;   // China (PLA) — Dalian default opposition
};

MapConquestLayout parse_map_conquest(const std::string& gameplay_script);
std::string control_point_label_from_name(const std::string& cp_name);

bool map_conquest_parser_self_test();

}  // namespace dalian
