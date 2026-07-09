#include "map_conquest_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace dalian {
namespace {

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

bool parse_triple(const std::string& triple, float out[3]) {
  float v[3]{};
  char slash = '/';
  std::istringstream ss(triple);
  if (!(ss >> v[0] >> slash >> v[1] >> slash >> v[2])) return false;
  out[0] = v[0];
  out[1] = v[1];
  out[2] = v[2];
  return true;
}

TeamId team_from_int(int t) {
  if (t == 1) return TeamId::Team1;
  if (t == 2) return TeamId::Team2;
  return TeamId::Neutral;
}

}  // namespace

std::string control_point_label_from_name(const std::string& cp_name) {
  std::string s = cp_name;
  const auto pos = s.rfind('_');
  if (pos != std::string::npos) s = s.substr(pos + 1);
  for (char& c : s) {
    if (c == '_') c = ' ';
    else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

MapConquestLayout parse_map_conquest(const std::string& gameplay_script) {
  MapConquestLayout layout;
  std::unordered_map<std::string, ParsedControlPoint> cp_templates;
  std::unordered_map<std::string, glm::vec3> cp_positions;

  std::string cur_template;
  bool in_cp_template = false;
  std::string pending_spawn_template;
  int pending_spawn_cp_id = 0;

  for (const std::string& line : split_lines(gameplay_script)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string lc = lower_copy(cmd);

    if (lc == "objecttemplate.create") {
      std::string type, name;
      ls >> type >> name;
      cur_template = name;
      in_cp_template = lower_copy(type) == "controlpoint";
      pending_spawn_template.clear();
      pending_spawn_cp_id = 0;
      if (in_cp_template) {
        ParsedControlPoint cp;
        cp.name = name;
        cp.label = control_point_label_from_name(name);
        cp_templates[name] = cp;
      } else if (lower_copy(type) == "spawnpoint") {
        pending_spawn_template = name;
        pending_spawn_cp_id = 0;
      }
      continue;
    }

    if (in_cp_template && !cur_template.empty()) {
      auto it = cp_templates.find(cur_template);
      if (it != cp_templates.end()) {
        if (lc == "objecttemplate.team") {
          int team = 0;
          ls >> team;
          it->second.initial_team = team_from_int(team);
        } else if (lc == "objecttemplate.controlpointid") {
          ls >> it->second.bf2_id;
        } else if (lc == "objecttemplate.radius") {
          ls >> it->second.radius;
        }
      }
    }

    if (!pending_spawn_template.empty() && lc == "objecttemplate.setcontrolpointid") {
      ls >> pending_spawn_cp_id;
      continue;
    }

    if (lc == "object.create") {
      std::string name;
      ls >> name;
      cur_template = name;
      continue;
    }

    if (lc == "object.absoluteposition" && !cur_template.empty()) {
      std::string triple;
      ls >> triple;
      float v[3];
      if (!parse_triple(triple, v)) continue;
      const glm::vec3 pos(v[0], v[1], v[2]);

      if (cp_templates.count(cur_template)) {
        cp_positions[cur_template] = pos;
      } else if (pending_spawn_cp_id != 0 &&
                 lower_copy(cur_template).find("cpname") != std::string::npos) {
        ParsedSoldierSpawn sp;
        sp.pos = pos;
        sp.bf2_cp_id = pending_spawn_cp_id;
        layout.soldier_spawns.push_back(sp);
        pending_spawn_cp_id = 0;
        pending_spawn_template.clear();
      }
    }
  }

  for (const auto& [name, tmpl] : cp_templates) {
    ParsedControlPoint cp = tmpl;
    const auto pit = cp_positions.find(name);
    if (pit == cp_positions.end()) continue;
    cp.pos = pit->second;
    if (cp.bf2_id != 0) layout.control_points.push_back(cp);
  }

  std::sort(layout.control_points.begin(), layout.control_points.end(),
            [](const ParsedControlPoint& a, const ParsedControlPoint& b) {
              return a.bf2_id < b.bf2_id;
            });

  return layout;
}

bool map_conquest_parser_self_test() {
  const char* sample = R"(
ObjectTemplate.create ControlPoint CPNAME_DP_16_powerplant
ObjectTemplate.team 1
ObjectTemplate.controlPointId 401
ObjectTemplate.radius 15
Object.create CPNAME_DP_16_powerplant
Object.absolutePosition 0/10/0
ObjectTemplate.create ControlPoint CPNAME_DP_16_constructionsite
ObjectTemplate.team 2
ObjectTemplate.controlPointId 402
ObjectTemplate.radius 10
Object.create CPNAME_DP_16_constructionsite
Object.absolutePosition 100/10/50
ObjectTemplate.create SpawnPoint CPNAME_DP_16_powerplant_1
ObjectTemplate.setControlPointId 401
Object.create CPNAME_DP_16_powerplant_1
Object.absolutePosition 1/11/1
)";
  const MapConquestLayout layout = parse_map_conquest(sample);
  if (layout.control_points.size() != 2) return false;
  if (layout.control_points[0].initial_team != TeamId::Team1) return false;
  if (layout.control_points[1].initial_team != TeamId::Team2) return false;
  if (layout.soldier_spawns.size() != 1) return false;
  return layout.soldier_spawns[0].bf2_cp_id == 401;
}

}  // namespace dalian
