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
  // Sun / moon (night maps use dim sunColor; moon from Skydome.flareDirection).
  glm::vec3 sun_dir{0.4f, -0.82f, -0.4f};
  glm::vec3 sun_color{1.3f, 1.2f, 1.05f};
  glm::vec3 sun_spec_color{0.9f, 0.88f, 0.82f};
  glm::vec3 moon_dir{0.f, 1.f, 0.f};  // direction toward moon in the sky
  bool has_moon = false;

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
  std::string flare_texture;
  glm::vec2 cloud_scroll{0.002f, 0.f};
  float cloud_fade_start = 800.f;
  float cloud_fade_end = 4000.f;
  float dome_rotation = 0.f;  // degrees → sky UV yaw offset
  bool has_cloud_layer = false;
  bool enable_sun = true;

  bool is_night = false;
  float water_sun_intensity = 1.f;

  // Water plane.
  bool has_water = false;
  float water_level = 0.f;
  glm::vec3 water_color{0.10f, 0.13f, 0.16f};
  glm::vec3 water_specular{0.72f, 0.62f, 0.51f};
  float water_specular_power = 12.f;
  glm::vec3 water_fog_color{0.69f, 0.73f, 0.80f};
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

inline void normalize_rgb_if_byte(glm::vec3& c) {
  if (c.r > 1.f || c.g > 1.f || c.b > 1.f) {
    c /= 255.f;
  }
}

inline void slash_to_fwd(std::string& s) {
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
}

}  // namespace detail

// water_con / sky_con / heightdata_con may be empty; sensible defaults are kept.
inline Atmosphere parse_atmosphere(const std::string& water_con, const std::string& sky_con,
                                   const std::string& heightdata_con) {
  Atmosphere a;
  std::string args;
  float v[3];
  bool have_sky_low = false;
  bool have_sky_high = false;
  glm::vec3 sky_low{0.2f, 0.25f, 0.35f};
  glm::vec3 sky_high{0.2f, 0.2f, 0.2f};

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
  if (detail::find_command(sky_con, "Lightmanager.skycolorLow", args) &&
      detail::parse_triple(args, v) == 3) {
    sky_low = glm::vec3(v[0], v[1], v[2]);
    have_sky_low = true;
  }
  if (detail::find_command(sky_con, "Lightmanager.skycolorHigh", args) &&
      detail::parse_triple(args, v) == 3) {
    sky_high = glm::vec3(v[0], v[1], v[2]);
    have_sky_high = true;
  }
  if (detail::find_command(sky_con, "Lightmanager.sunSpecColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.sun_spec_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "LightSettings.TerrainSunColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.terrain_sun_color = glm::vec3(v[0], v[1], v[2]);
  }
  // Non-editor branch writes terrain.sunColor instead of LightSettings.*.
  if (detail::find_command(sky_con, "terrain.sunColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.terrain_sun_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "LightSettings.TerrainSkyColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.terrain_sky_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "terrain.GIColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.terrain_sky_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(sky_con, "Renderer.fogColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.fog_color = glm::vec3(v[0], v[1], v[2]);
    detail::normalize_rgb_if_byte(a.fog_color);
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
  if (detail::find_command(sky_con, "LightmapSettings.waterSunIntensity", args) ||
      detail::find_command(sky_con, "terrain.waterSunIntensity", args)) {
    std::istringstream is(args);
    is >> a.water_sun_intensity;
  }
  if (detail::find_command(sky_con, "Lightmanager.enableSun", args)) {
    int en = 1;
    std::istringstream is(args);
    if (is >> en) a.enable_sun = (en != 0);
  }
  if (detail::find_command(sky_con, "Skydome.skyTexture", args)) {
    a.sky_texture = args;
    detail::slash_to_fwd(a.sky_texture);
  }
  // BF2 uses cloudTexture / cloudTexture2 (not cloudTexture(2)).
  if (detail::find_command(sky_con, "Skydome.cloudTexture", args)) {
    a.cloud_texture = args;
    detail::slash_to_fwd(a.cloud_texture);
    a.has_cloud_layer = !a.cloud_texture.empty();
  }
  if (detail::find_command(sky_con, "Skydome.hasCloudLayer", args)) {
    int h = 0;
    std::istringstream is(args);
    if (is >> h) a.has_cloud_layer = (h != 0) && !a.cloud_texture.empty();
  }
  if (detail::find_command(sky_con, "Skydome.scrolldirection", args) &&
      detail::parse_triple(args, v) >= 2) {
    a.cloud_scroll = glm::vec2(v[0], v[1]);
  }
  if (detail::find_command(sky_con, "Skydome.domeRotation", args)) {
    std::istringstream is(args);
    is >> a.dome_rotation;
  }
  if (detail::find_command(sky_con, "Skydome.flareTexture", args)) {
    a.flare_texture = args;
    detail::slash_to_fwd(a.flare_texture);
  }
  if (detail::find_command(sky_con, "Skydome.flareDirection", args) &&
      detail::parse_triple(args, v) == 3) {
    a.moon_dir = glm::normalize(glm::vec3(v[0], v[1], v[2]));
    a.has_moon = true;
  }
  if (detail::find_command(sky_con, "Skydome.fadeCloudsDistances", args) &&
      detail::parse_triple(args, v) >= 2) {
    a.cloud_fade_start = v[0];
    a.cloud_fade_end = v[1];
  }

  const float sun_avg = (a.sun_color.r + a.sun_color.g + a.sun_color.b) / 3.f;
  const float sky_avg = (a.sky_color.r + a.sky_color.g + a.sky_color.b) / 3.f;
  const float terrain_sun_avg =
      (a.terrain_sun_color.r + a.terrain_sun_color.g + a.terrain_sun_color.b) / 3.f;
  const std::string sky_tex_l = detail::lower_copy(a.sky_texture);
  const bool night_in_name =
      sky_tex_l.find("night") != std::string::npos || sky_tex_l.find("moon") != std::string::npos;
  // XPack night maps often keep Lightmanager.skycolor at 1/1/1 (fake-HDR) while
  // sun/terrain sun are dim and the sky texture name contains "night".
  a.is_night = (!a.enable_sun) || (sun_avg < 0.45f && sky_avg < 0.35f) || (sky_avg < 0.25f) ||
               (sun_avg < 0.4f && terrain_sun_avg < 0.08f) || night_in_name ||
               (a.has_moon && sun_avg < 0.5f && terrain_sun_avg < 0.15f);

  if (a.is_night) {
    // Prefer authored low/high sky colours over the fake-HDR white skycolor.
    if (have_sky_low || have_sky_high) {
      a.sky_color = have_sky_high ? sky_high : sky_low;
      a.horizon_color = have_sky_low ? sky_low : glm::mix(a.sky_color, a.fog_color, 0.55f);
    } else {
      a.sky_color = a.terrain_sky_color;
      a.horizon_color = glm::mix(a.terrain_sky_color, a.fog_color, 0.55f);
    }
    // Cool moonlight tint when the map only authored a dim warm-ish sunColor.
    if (sun_avg > 0.01f && a.sun_color.b < a.sun_color.r * 1.05f) {
      a.sun_color = glm::vec3(0.35f, 0.42f, 0.65f) * glm::max(sun_avg, 0.15f) * 2.2f;
    }
  } else {
    a.horizon_color = glm::mix(a.sky_color, a.fog_color, 0.55f);
  }

  // --- Water.con: water tint (0-1). ---
  if (detail::find_command(water_con, "renderer.waterColor", args) &&
      detail::parse_triple(args, v) == 3) {
    a.water_color = glm::vec3(v[0], v[1], v[2]);
  }
  if (detail::find_command(water_con, "renderer.waterSpecularColor", args)) {
    float r = 0.f, g = 0.f, b = 0.f, power = 12.f;
    std::istringstream is(args);
    char slash = 0;
    if (is >> r >> slash >> g >> slash >> b) {
      a.water_specular = glm::vec3(r, g, b);
      if (is >> slash >> power) a.water_specular_power = power;
    }
  }
  if (detail::find_command(water_con, "renderer.waterFogColor", args)) {
    float r = 0.f, g = 0.f, b = 0.f;
    std::istringstream is(args);
    char slash = 0;
    if (is >> r >> slash >> g >> slash >> b) {
      // BF2 stores these as 0–255 in Water.con.
      if (r > 1.f || g > 1.f || b > 1.f) {
        r /= 255.f;
        g /= 255.f;
        b /= 255.f;
      }
      a.water_fog_color = glm::vec3(r, g, b);
    }
  }
  if (detail::find_command(water_con, "terrain.waterSunIntensity", args) ||
      detail::find_command(water_con, "LightmapSettings.waterSunIntensity", args)) {
    float s = 1.f;
    std::istringstream is(args);
    if (is >> s) a.water_sun_intensity = s;
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
