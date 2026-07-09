#include "static_object_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string_view>

namespace bf2 {
namespace {

std::string trim(std::string_view s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

void parse_vec3(const std::string& token, float out[3]) {
  std::size_t start = 0;
  for (int i = 0; i < 3; ++i) {
    const auto slash = token.find('/', start);
    const std::string part = token.substr(start, slash - start);
    try {
      out[i] = part.empty() ? 0.f : std::stof(part);
    } catch (...) {
      out[i] = 0.f;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
}

std::string normalize_run_path(std::string path) {
  for (auto& c : path) {
    if (c == '\\') c = '/';
  }
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  const std::string prefix = "objects/";
  if (lower(path).rfind(prefix, 0) == 0) path.erase(0, prefix.size());
  return path;
}

}  // namespace

void StaticObjectParser::clear() {
  entities_.clear();
  run_paths_.clear();
}

glm::quat StaticObjectParser::rotation_from_euler_deg(const glm::vec3& yaw_pitch_roll) {
  const glm::quat q_yaw = glm::angleAxis(glm::radians(yaw_pitch_roll.x), glm::vec3(0.f, 1.f, 0.f));
  const glm::quat q_pitch = glm::angleAxis(glm::radians(yaw_pitch_roll.y), glm::vec3(1.f, 0.f, 0.f));
  const glm::quat q_roll = glm::angleAxis(glm::radians(yaw_pitch_roll.z), glm::vec3(0.f, 0.f, 1.f));
  return q_yaw * q_pitch * q_roll;
}

void StaticObjectParser::parse(const std::string& static_objects_script) {
  clear();
  std::istringstream stream(static_objects_script);
  std::string line;
  while (std::getline(stream, line)) {
    line = trim(line);
    if (line.empty()) continue;
    if (line.rfind("rem", 0) == 0 || line.front() == ';') continue;

    if (line.rfind("run ", 0) == 0) {
      std::istringstream ls(line.substr(4));
      std::string path;
      if (ls >> path) run_paths_.push_back(normalize_run_path(path));
      continue;
    }

    std::istringstream ls(line);
    std::string cmd, arg;
    if (!(ls >> cmd)) continue;

    if (cmd == "Object.create") {
      StaticEntity ent;
      if (ls >> arg) ent.template_name = arg;
      entities_.push_back(std::move(ent));
      continue;
    }
    if (entities_.empty()) continue;

    StaticEntity& ent = entities_.back();
    if (cmd == "Object.absolutePosition" && (ls >> arg)) {
      float p[3]{};
      parse_vec3(arg, p);
      ent.position = {p[0], p[1], p[2]};
    } else if (cmd == "Object.rotation" && (ls >> arg)) {
      float r[3]{};
      parse_vec3(arg, r);
      ent.rotation_euler_deg = {r[0], r[1], r[2]};
      ent.rotation = rotation_from_euler_deg(ent.rotation_euler_deg);
    } else if (cmd == "Object.layer" && (ls >> arg)) {
      try {
        ent.layer = std::stoi(arg);
      } catch (...) {
      }
    }
  }
}

std::vector<std::string> StaticObjectParser::run_paths() const { return run_paths_; }

bool static_object_parser_self_test() {
  static const char* kSample = R"(
run /objects/staticobjects/industry/ind_buildings/lrgfactorybuilding/lrgfactorybuilding.con
rem *** lrgfactorybuilding ***
Object.create lrgfactorybuilding
Object.absolutePosition -59.987/176.238/-269.162
Object.layer 1
Object.create coolingtower_01
Object.absolutePosition -26.928/188.040/-163.616
Object.rotation -66.6/0.0/0.0
)";

  StaticObjectParser parser;
  parser.parse(kSample);
  if (parser.entities().size() != 2) return false;
  if (parser.run_paths().size() != 1) return false;
  if (parser.run_paths()[0].find("lrgfactorybuilding.con") == std::string::npos) return false;
  const auto& a = parser.entities()[0];
  if (std::fabs(a.position.x + 59.987f) > 1e-3f) return false;
  if (std::fabs(a.position.y - 176.238f) > 1e-3f) return false;
  const auto& b = parser.entities()[1];
  if (std::fabs(b.rotation_euler_deg.x + 66.6f) > 1e-3f) return false;
  return true;
}

}  // namespace bf2
