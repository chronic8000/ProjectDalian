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
  // Sun / moon (night maps use dim sunColor).
  glm::vec3 sun_dir{0.4f, -0.82f, -0.4f};
  glm::vec3 sun_color{1.3f, 1.2f, 1.05f};
  glm::vec3 sun_spec_color{0.9f, 0.88f, 0.82f};

  // Sky gradient.
  glm::vec3 sky_color{0.35f, 0.55f, 0.85f};
  glm::vec3 horizon_color{0.78f, 0.82f, 0.88f};
  glm::vec3 terrain_sun_color{1.f, 0.95f, 0.85f};
  glm::vec3 terrain_sky_color{0.5f, 0.6f, 0.75f};

  // Fog.
  float fog_start = 350.f;
  float fog_end = 1600.f;
  glm::vec3 fog_color{0.78f, 0.82f, 0.88f};

  // Skydome / clouds (texture vpaths without extension).
  std::string sky_texture;
  std::string cloud_texture;
  glm::vec2 cloud_scroll{0.002f, 0.f};
  float cloud_fade_start = 800.f;
  float cloud_fade_end = 4000.f;
  bool has_cloud_layer = false;

  bool is_night = false;
  float water_sun_intensity = 1.f;

  // Water plane.
  bool has_water = false;
  float water_level = 0.f;
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
  if (detail::find_command(sky_con, "Lightmanager.sunSpecColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.sun_spec_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "LightSettings.TerrainSunColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.terrain_sun_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "LightSettings.TerrainSkyColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.terrain_sky_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "Renderer.fogColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.fog_color = glm::vec3(v[0], v[1], v[2]);
    a.horizon_color = a.fog_color;
  }
  if (detail::find_command(sky_con, "Renderer.fogStartEndAndBase", args)) {
    float fs = 0.f, fe = 0.f, fb = 0.f;
    std::istringstream is(args);
    if (is >> fs >> fe >> fb) {
      a.fog_start = fs;
      a.fog_end = fe;
    }
  }
  if (detail::find_command(sky_con, "LightmapSettings.waterSunIntensity", args)) {
    std::istringstream is(args);
    is >> a.water_sun_intensity;
  }
  if (detail::find_command(sky_con, "Skydome.skyTexture", args)) {
    a.sky_texture = args;
    for (char& c : a.sky_texture) {
      if (c == '\\') c = '/';
    }
  }
  if (detail::find_command(sky_con, "Skydome.cloudTexture(2)", args) ||
      detail::find_command(sky_con, "Skydome.cloudTexture", args)) {
    a.cloud_texture = args;
    for (char& c : a.cloud_texture) {
      if (c == '\\') c = '/';
    }
    a.has_cloud_layer = !a.cloud_texture.empty();
  }
  if (detail::find_command(sky_con, "Skydome.scrolldirection(2)", args) &&
      detail::parse_triple(args, v) >= 2) {
    a.cloud_scroll = glm::vec2(v[0], v[1]);
  }
  const float sun_avg = (a.sun_color.r + a.sun_color.g + a.sun_color.b) / 3.f;
  const float sky_avg = (a.sky_color.r + a.sky_color.g + a.sky_color.b) / 3.f;
  a.is_night = sun_avg < 0.45f && sky_avg < 0.35f;
  if (!a.is_night && sky_avg < 0.25f) a.is_night = true;
  a.horizon_color = glm::mix(a.sky_color, a.fog_color, 0.55f);

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
