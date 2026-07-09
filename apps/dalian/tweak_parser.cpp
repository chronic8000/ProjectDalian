#include "tweak_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string_view>

namespace dalian {
namespace {

std::string trim(std::string_view s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

std::string unquote(std::string_view s) {
  if (s.size() >= 2 &&
      ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    return std::string(s.substr(1, s.size() - 2));
  }
  return std::string(s);
}

bool parse_vec3_token(std::string_view token, glm::vec3& out) {
  std::string t(token);
  for (char& c : t) {
    if (c == '/') c = ' ';
  }
  std::istringstream iss(t);
  float x = 0.f, y = 0.f, z = 0.f;
  if (!(iss >> x >> y >> z)) return false;
  out = {x, y, z};
  return true;
}

}  // namespace

void TweakData::clear() { entries_.clear(); }

void TweakData::parse(const std::string& text) {
  clear();
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    line = trim(line);
    if (line.empty()) continue;
    if (line[0] == '#' || (line.size() > 1 && line[0] == '/' && line[1] == '/')) continue;
    const std::size_t tab = line.find('\t');
    const std::size_t sp = line.find(' ');
    std::size_t split = std::string::npos;
    if (tab != std::string::npos) split = tab;
    if (sp != std::string::npos) split = (split == std::string::npos) ? sp : std::min(split, sp);
    if (split == std::string::npos) continue;
    std::string key = trim(line.substr(0, split));
    std::string value = unquote(trim(line.substr(split + 1)));
    if (key.empty()) continue;
    entries_[key] = value;
  }
}

bool TweakData::has(const std::string& key) const { return entries_.find(key) != entries_.end(); }

std::string TweakData::get_string(const std::string& key, const std::string& default_val) const {
  const auto it = entries_.find(key);
  return it != entries_.end() ? it->second : default_val;
}

float TweakData::get_float(const std::string& key, float default_val) const {
  const auto it = entries_.find(key);
  if (it == entries_.end()) return default_val;
  try {
    return std::stof(it->second);
  } catch (...) {
    return default_val;
  }
}

int TweakData::get_int(const std::string& key, int default_val) const {
  const auto it = entries_.find(key);
  if (it == entries_.end()) return default_val;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return default_val;
  }
}

glm::vec3 TweakData::get_vec3(const std::string& key, const glm::vec3& default_val) const {
  const auto it = entries_.find(key);
  if (it == entries_.end()) return default_val;
  glm::vec3 out;
  if (parse_vec3_token(it->second, out)) return out;
  return default_val;
}

bool tweak_parser_self_test() {
  static const char* kSample = R"(
# Weapon tweak fragment
ObjectTemplate.create Weapon
ObjectTemplate.activeSafe Weapon
ObjectTemplate.fire.deviation 0.5
ObjectTemplate.fire.minDeviation 0.02
ObjectTemplate.fire.maxDeviation 1.8
ObjectTemplate.position 12.5/3.0/-8.25
ObjectTemplate.soundFilename "sounds/weapons/fire.wav"
ObjectTemplate.magazineSize 30
)";

  TweakData data;
  data.parse(kSample);

  if (!data.has("ObjectTemplate.fire.deviation")) return false;
  if (std::fabs(data.get_float("ObjectTemplate.fire.deviation", -1.f) - 0.5f) > 1e-5f) return false;
  if (data.get_float("ObjectTemplate.fire.missing", 9.f) != 9.f) return false;
  if (data.get_int("ObjectTemplate.magazineSize", 0) != 30) return false;

  const glm::vec3 pos = data.get_vec3("ObjectTemplate.position");
  if (std::fabs(pos.x - 12.5f) > 1e-4f || std::fabs(pos.y - 3.f) > 1e-4f ||
      std::fabs(pos.z + 8.25f) > 1e-4f) {
    return false;
  }

  if (data.get_string("ObjectTemplate.soundFilename") != "sounds/weapons/fire.wav") return false;
  return true;
}

}  // namespace dalian
