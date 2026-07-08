#pragma once

// Parses a BF2 level's sky/water/lighting settings (Sky.con, Water.con,
// Heightdata.con) into a small struct the renderer can consume for the gradient
// sky, distance fog, sun direction and the translucent water plane.

#include <glm/glm.hpp>

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace bf2 {

struct Atmosphere {
  // Sun.
  glm::vec3 sun_dir{0.4f, -0.82f, -0.4f};  // direction the light travels (down)
  glm::vec3 sun_color{1.3f, 1.2f, 1.05f};

  // Sky gradient.
  glm::vec3 sky_color{0.35f, 0.55f, 0.85f};      // zenith
  glm::vec3 horizon_color{0.78f, 0.82f, 0.88f};  // horizon / fog colour

  // Distance fog (world metres). fog_end <= 0 disables fog.
  float fog_start = 350.f;
  float fog_end = 1600.f;

  // Water plane.
  bool has_water = false;
  float water_level = 0.f;  // world Y
  glm::vec3 water_color{0.10f, 0.13f, 0.16f};
};

namespace detail {

inline std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Parse "a/b/c" (or "a b c") into up to three floats. Returns count parsed.
inline int parse_triple(const std::string& s, float out[3]) {
  std::string t = s;
  for (char& c : t) {
    if (c == '/') c = ' ';
  }
  std::istringstream is(t);
  int n = 0;
  while (n < 3 && (is >> out[n])) ++n;
  return n;
}

// Find the first line whose first token equals `key` (case-insensitive) and
// return the remainder of the line (arguments). Returns empty when not found.
inline bool find_command(const std::string& script, const std::string& key, std::string& args_out) {
  const std::string want = lower_copy(key);
  std::istringstream lines(script);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream ls(line);
    std::string tok;
    if (!(ls >> tok)) continue;
    if (lower_copy(tok) == want) {
      std::string rest;
      std::getline(ls, rest);
      // trim leading spaces
      std::size_t p = rest.find_first_not_of(" \t\r");
      args_out = (p == std::string::npos) ? std::string() : rest.substr(p);
      return true;
    }
  }
  return false;
}

}  // namespace detail

// water_con / sky_con / heightdata_con may be empty; sensible defaults are kept.
inline Atmosphere parse_atmosphere(const std::string& water_con, const std::string& sky_con,
                                   const std::string& heightdata_con) {
  Atmosphere a;
  std::string args;
  float v[3];

  // --- Sky.con: sun direction/colour + sky colour. ---
  if (detail::find_command(sky_con, "Lightmanager.sunDirection", args) &&
      detail::parse_triple(args, v) == 3) {
    a.sun_dir = glm::normalize(glm::vec3(v[0], v[1], v[2]));
  }
  if (detail::find_command(sky_con, "Lightmanager.sunColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.sun_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "Lightmanager.skycolor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.sky_color = glm::vec3(v[0], v[1], v[2]);
  }
  // Horizon/fog colour: a paler version of the zenith sky for an atmospheric haze.
  a.horizon_color = glm::mix(a.sky_color, glm::vec3(0.9f, 0.92f, 0.95f), 0.55f);

  // --- Water.con: water tint (0-1). ---
  if (detail::find_command(water_con, "renderer.waterColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.water_color = glm::vec3(v[0], v[1], v[2]);
  }

  // --- Heightdata.con: sea level (world Y). ---
  if (detail::find_command(heightdata_con, "heightmapcluster.setSeaWaterLevel", args)) {
    float lvl = 0.f;
    std::istringstream is(args);
    if (is >> lvl) {
      a.water_level = lvl;
      a.has_water = true;
    }
  }

  return a;
}

}  // namespace bf2
