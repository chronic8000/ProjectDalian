// Project Dalian: a modern tactical war-sim built on real BF2 (Refractor 2)
// assets. Successor to the "Dalian Plant" map that gave the project its name.
//
// Mounts a level's server.zip (+ the mod's Objects_client.zip), builds the
// terrain, resolves static-object placements to textured meshes, and drops you
// in as a soldier with animated enemies, ballistics, an FPV drone and shadows.
//
//   project_dalian <level_server.zip> [objects_client.zip]
//
// Controls: retail BF2 defaults (rebindable in Options). Dalian extras on F9/F10/H.
// ESC pause menu (Alt+F4 to quit).
#include <GL/glew.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "app_settings.hpp"
#include "key_bindings.hpp"
#include "loading_screen.hpp"
#include "bot_names.hpp"
#include "hud_feed.hpp"
#include "mp_sync.hpp"
#include "ui_layout.hpp"
#include "factions.hpp"
#include "game_audio.hpp"
#include "menu.hpp"
#include "server_discovery.hpp"
#include "conquest_voice.hpp"
#include "game_sim.hpp"
#include "game_snapshot.hpp"
#include "game_logic_parser.hpp"
#include "map_conquest_parser.hpp"
#include "minimap_projector.hpp"
#include "bf2_effects.hpp"
#include "vehicle_classify.hpp"
#include "weapon_profile.hpp"
#include "soldier_anim.hpp"
#include "soldier_anim_library.hpp"
#include "vehicle_tweak_profile.hpp"
#include "vehicle_wing_profile.hpp"
#include "vehicle_air_profile.hpp"
#include "vehicle_weapon_profile.hpp"
#include "projectile_profile.hpp"
#include "ambient_emitter.hpp"
#include "engine/formats/effects/effect_bundle.hpp"
#include "engine/formats/nav/bf2_nav_mesh.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <cstdio>
#include <utility>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <unordered_set>
#include <vector>

#include "engine/anim/pose.hpp"
#include "engine/anim/skinning.hpp"
#include "engine/core/asset_audit.hpp"
#include "engine/core/atmosphere.hpp"
#include "engine/core/level_loader.hpp"
#include "engine/core/collision_resolver.hpp"
#include "engine/core/mesh_bounds.hpp"
#include "engine/core/placement_snap.hpp"
#include "engine/core/archive_path_resolve.hpp"
#include "engine/core/overgrowth_instances.hpp"
#include "engine/core/object_lightmaps.hpp"
#include "engine/core/undergrowth.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"
#include "engine/net/net.hpp"
#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/collision/bf2_collision.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/formats/mesh/bf2_road_mesh.hpp"
#include "engine/formats/mesh/obj_loader.hpp"
#include "engine/formats/terrain/terrain_colormap.hpp"
#include "engine/physics/drone.hpp"
#include "engine/physics/kamikaze_drone.hpp"
#include "engine/physics/missile.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/render/cascaded_shadow_maps.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/texture_cache.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

std::string sibling_client_zip(const std::string& server_zip) {
  const auto p = std::filesystem::path(server_zip);
  if (!p.has_parent_path()) {
    return {};
  }
  const auto candidate = p.parent_path() / "client.zip";
  return std::filesystem::exists(candidate) ? candidate.string() : std::string();
}

// Build a renderable grid mesh from a terrain heightfield, centred on the world
// origin to match BF2 StaticObjects.con placement coordinates.
bf2::ExtractedMesh terrain_to_mesh(const bf2::Terrain& t, float xz_scale, int step) {
  bf2::ExtractedMesh mesh;
  const int w = static_cast<int>(t.width);
  const int h = static_cast<int>(t.height);
  const int gw = (w + step - 1) / step;
  const int gh = (h + step - 1) / step;
  const float off_x = ((w - 1.f) * 0.5f) * xz_scale;
  const float off_z = ((h - 1.f) * 0.5f) * xz_scale;

  auto height_at = [&](int x, int z) -> float {
    x = std::clamp(x, 0, w - 1);
    z = std::clamp(z, 0, h - 1);
    return t.samples[static_cast<std::size_t>(z) * w + x].height;
  };

  mesh.vertices.reserve(static_cast<std::size_t>(gw) * gh);
  for (int gz = 0; gz < gh; ++gz) {
    for (int gx = 0; gx < gw; ++gx) {
      const int x = gx * step;
      const int z = gz * step;
      bf2::ExtractedVertex v;
      v.position.x = x * xz_scale - off_x;
      v.position.y = height_at(x, z);
      v.position.z = z * xz_scale - off_z;
      const float hl = height_at(x - step, z);
      const float hr = height_at(x + step, z);
      const float hd = height_at(x, z - step);
      const float hu = height_at(x, z + step);
      glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * step * xz_scale, hd - hu));
      v.normal = {n.x, n.y, n.z};
      mesh.vertices.push_back(v);
    }
  }

  mesh.indices.reserve(static_cast<std::size_t>(gw - 1) * (gh - 1) * 6);
  for (int gz = 0; gz < gh - 1; ++gz) {
    for (int gx = 0; gx < gw - 1; ++gx) {
      const std::uint32_t a = gz * gw + gx;
      const std::uint32_t b = a + 1;
      const std::uint32_t c = a + gw;
      const std::uint32_t d = c + 1;
      mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
    }
  }
  return mesh;
}

// BF2 placements store rotation as yaw/pitch/roll in degrees.
// BF2 is left-handed: X east, Y up, Z north; yaw 0 = due north, +yaw = clockwise.
glm::mat4 placement_matrix(const bf2::ObjectInstance& inst) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f),
                               glm::vec3(inst.position[0], inst.position[1], inst.position[2]));
  m = glm::rotate(m, glm::radians(inst.rotation[0]), glm::vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(inst.rotation[1]), glm::vec3(1, 0, 0));
  m = glm::rotate(m, glm::radians(inst.rotation[2]), glm::vec3(0, 0, 1));
  return m;
}

// Look direction from BF2 yaw/pitch (degrees). yaw 0 = +Z (north).
glm::vec3 bf2_forward(float yaw_deg, float pitch_deg) {
  const float y = glm::radians(yaw_deg);
  const float p = glm::radians(pitch_deg);
  const float cp = std::cos(p);
  return glm::normalize(glm::vec3(std::sin(y) * cp, std::sin(p), std::cos(y) * cp));
}

// Strafe axis for BF2 (left-handed): up × forward. RH cross(forward, up) mirrors L/R.
glm::vec3 bf2_right(const glm::vec3& flat_forward) {
  return glm::normalize(glm::cross(glm::vec3(0.f, 1.f, 0.f), flat_forward));
}

std::string mesh_texture_folder(const std::string& vpath) {
  return bf2::mesh_texture_folder_hint(vpath);
}

bool is_track_texture_path(const std::string& map_path) {
  std::string l = map_path;
  for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // Explicit tread maps, plus BF2's common "*_w.dds" wheel/track atlas sheet
  // (e.g. tnk_type98_w.dds) used by animated-UV tread materials.
  if (l.find("track") != std::string::npos) return true;
  const auto slash = l.find_last_of('/');
  const std::string base = slash == std::string::npos ? l : l.substr(slash + 1);
  return base.size() > 6 && base.find("_w.") != std::string::npos;
}

bool vehicle_path_is_tracked(const std::string& vpath) {
  std::string l = vpath;
  for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // Path fallback when tweak is unavailable — tanks/SPG only, not wheeled APCs.
  return l.find("/tnk_") != std::string::npos || l.find("/ustnk_") != std::string::npos ||
         l.find("/plz") != std::string::npos;
}

bool vehicle_tweak_is_tracked(const std::string& tweak_text) {
  std::string l = tweak_text;
  for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return l.find("setenginetype c_ettank") != std::string::npos;
}

// BF2 tanks: animatedUVTranslationIndex marks tread-belt UV matrices that linear-
// scroll; animatedUVRotationIndex marks wheel-pad matrices that must NOT scroll
// (linear scroll smears hull camo into the track loop). TranslationMax x/y picks axis.
struct TrackUvMatrixInfo {
  std::unordered_map<std::uint8_t, bool> translate_axis_v;  // id -> scroll on V
  std::unordered_set<std::uint8_t> rotation_ids;
};

TrackUvMatrixInfo parse_track_uv_matrices(const std::string& tweak_text) {
  TrackUvMatrixInfo out;
  std::istringstream in(tweak_text);
  std::string line;
  float pending_max_u = 0.f, pending_max_v = 0.f;
  bool have_max = false;
  auto lc = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd, arg;
    if (!(ls >> cmd >> arg)) continue;
    const std::string k = lc(cmd);
    if (k == "objecttemplate.animateduvtranslationmax") {
      pending_max_u = pending_max_v = 0.f;
      have_max = false;
      const auto slash = arg.find('/');
      try {
        pending_max_u = std::stof(arg.substr(0, slash));
        if (slash != std::string::npos) pending_max_v = std::stof(arg.substr(slash + 1));
        have_max = true;
      } catch (...) {
      }
    } else if (k == "objecttemplate.animateduvtranslationindex") {
      try {
        const int id = std::stoi(arg);
        if (id > 0 && id < 256) {
          const bool axis_v =
              have_max ? (std::fabs(pending_max_v) >= std::fabs(pending_max_u)) : true;
          out.translate_axis_v[static_cast<std::uint8_t>(id)] = axis_v;
        }
      } catch (...) {
      }
    } else if (k == "objecttemplate.animateduvrotationindex") {
      try {
        const int id = std::stoi(arg);
        if (id >= 0 && id < 256) out.rotation_ids.insert(static_cast<std::uint8_t>(id));
      } catch (...) {
      }
    }
  }
  return out;
}

void apply_track_uv_matrices(bf2::TexturedMeshData& data, const TrackUvMatrixInfo& info) {
  auto submesh_min_u = [&](const bf2::TexturedSubmesh& sm) {
    float min_u = 1e9f;
    for (std::uint32_t k = 0; k < sm.index_count; ++k) {
      const auto idx = data.indices[sm.index_offset + k];
      if (idx < data.vertices.size()) min_u = std::min(min_u, data.vertices[idx].uv[0]);
    }
    return min_u;
  };

  for (auto& sm : data.submeshes) {
    if (sm.omit_from_hull || sm.uv_matrix_id == 0) {
      // Matrix 0 is static atlas UVs (hull/turret). Never scroll it — high-U
      // samples there are still hull detail, not the tread translation matrix.
      sm.animated_uv = false;
      continue;
    }
    // Whole submesh must sit in the high-U tread strip (min, not max).
    const bool tread_strip = submesh_min_u(sm) >= 0.85f;
    if (!tread_strip) {
      // UV-rotation / mid-U translation pads — keep tris for wheel hubs, skip hull.
      sm.omit_from_hull = true;
      sm.animated_uv = false;
      continue;
    }
    const auto it = info.translate_axis_v.find(sm.uv_matrix_id);
    if (it != info.translate_axis_v.end()) {
      sm.animated_uv = true;
      sm.animated_uv_axis_v = it->second;
    } else if (info.rotation_ids.count(sm.uv_matrix_id)) {
      sm.omit_from_hull = true;
      sm.animated_uv = false;
    } else if (info.translate_axis_v.empty()) {
      sm.animated_uv = true;
      sm.animated_uv_axis_v = true;
    }
  }
}

void fill_track_uv_strip(bf2::GpuSubmesh& gpu, const bf2::TexturedMeshData& data,
                         const bf2::TexturedSubmesh& sm) {
  if (!sm.animated_uv || sm.index_count == 0) return;
  float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
  for (std::uint32_t k = 0; k < sm.index_count; ++k) {
    const auto idx = data.indices[sm.index_offset + k];
    if (idx >= data.vertices.size()) continue;
    const auto& uv = data.vertices[idx].uv;
    umin = std::min(umin, uv[0]);
    umax = std::max(umax, uv[0]);
    vmin = std::min(vmin, uv[1]);
    vmax = std::max(vmax, uv[1]);
  }
  if (umax > umin && vmax > vmin) {
    gpu.track_uv_wrap_strip = true;
    gpu.track_strip_umin = umin;
    gpu.track_strip_umax = umax;
    gpu.track_strip_vmin = vmin;
    gpu.track_strip_vmax = vmax;
  }
}

// Drop static camo tris that sit inside the animated tread volume. Must run only
// after hierarchy xforms so positions are vehicle-space (part-local AABBs wrongly
// overlap the turret and delete armor).
void strip_assembled_track_camo(bf2::TexturedMeshData& data) {
  float tx0 = 1e9f, tx1 = -1e9f, ty0 = 1e9f, ty1 = -1e9f, tz0 = 1e9f, tz1 = -1e9f;
  bool have = false;
  for (const auto& sm : data.submeshes) {
    if (!sm.animated_uv) continue;
    for (std::uint32_t k = 0; k < sm.index_count; ++k) {
      const auto idx = data.indices[sm.index_offset + k];
      if (idx >= data.vertices.size()) continue;
      const auto& p = data.vertices[idx].position;
      have = true;
      tx0 = std::min(tx0, p.x);
      tx1 = std::max(tx1, p.x);
      ty0 = std::min(ty0, p.y);
      ty1 = std::max(ty1, p.y);
      tz0 = std::min(tz0, p.z);
      tz1 = std::max(tz1, p.z);
    }
  }
  if (!have) return;
  constexpr float pad = 0.05f;
  tx0 -= pad;
  tx1 += pad;
  ty0 -= pad;
  ty1 += pad;
  tz0 -= pad;
  tz1 += pad;

  std::vector<std::uint32_t> new_indices;
  std::vector<bf2::TexturedSubmesh> new_subs;
  new_indices.reserve(data.indices.size());
  for (const auto& sm : data.submeshes) {
    bf2::TexturedSubmesh out = sm;
    out.index_offset = static_cast<std::uint32_t>(new_indices.size());
    out.index_count = 0;
    for (std::uint32_t k = 0; k + 2 < sm.index_count; k += 3) {
      const std::uint32_t ia = data.indices[sm.index_offset + k];
      const std::uint32_t ib = data.indices[sm.index_offset + k + 1];
      const std::uint32_t ic = data.indices[sm.index_offset + k + 2];
      if (ia >= data.vertices.size() || ib >= data.vertices.size() ||
          ic >= data.vertices.size()) {
        continue;
      }
      if (!sm.animated_uv) {
        const float minu = std::min({data.vertices[ia].uv[0], data.vertices[ib].uv[0],
                                     data.vertices[ic].uv[0]});
        auto inside = [&](std::uint32_t i) {
          const auto& p = data.vertices[i].position;
          return p.x >= tx0 && p.x <= tx1 && p.y >= ty0 && p.y <= ty1 && p.z >= tz0 &&
                 p.z <= tz1;
        };
        if (minu < 0.85f && inside(ia) && inside(ib) && inside(ic)) continue;
      }
      new_indices.push_back(ia);
      new_indices.push_back(ib);
      new_indices.push_back(ic);
      out.index_count += 3;
    }
    if (out.index_count > 0) new_subs.push_back(std::move(out));
  }
  data.indices = std::move(new_indices);
  data.submeshes = std::move(new_subs);
}

std::string read_vehicle_tweak_text(bf2::ResourceManager& resources, const std::string& vpath) {
  std::string tweak_text;
  const auto meshes_pos = vpath.rfind("/meshes/");
  if (meshes_pos == std::string::npos) return tweak_text;
  const std::string folder = vpath.substr(0, meshes_pos);
  const std::string name = bf2::detail::basename_no_ext(vpath);
  for (const char* ext : {".tweak", ".con"}) {
    if (const auto tb = resources.read_bytes(folder + "/" + name + ext)) {
      tweak_text.append(reinterpret_cast<const char*>(tb->data()), tb->size());
      tweak_text.push_back('\n');
    }
  }
  return tweak_text;
}

// Load (or fetch from cache) a textured GPU mesh and wire up its material textures.
bf2::GpuTexturedMesh& ensure_textured_mesh(
    const std::string& vpath, std::unordered_map<std::string, bf2::GpuTexturedMesh>& cache,
    bf2::ResourceManager& resources, bf2::TextureCache& textures, bf2::Renderer& renderer) {
  auto it = cache.find(vpath);
  if (it == cache.end()) {
    bf2::GpuTexturedMesh gpu{};
    try {
      const auto m = resources.load_mesh(vpath);
      const auto data = bf2::MeshLoader::extract_textured(m, 0, 0);
      if (!data.vertices.empty()) {
        gpu = renderer.upload_textured(data);
        const std::string tex_folder = mesh_texture_folder(vpath);
        for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
          gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map, tex_folder);
          gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map, tex_folder);
          gpu.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map, tex_folder);
          gpu.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map, tex_folder);
          gpu.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map, tex_folder);
          gpu.submeshes[i].specular_tex = textures.get(data.submeshes[i].specular_map, tex_folder);
          gpu.submeshes[i].cutout = textures.is_cutout(gpu.submeshes[i].base_tex);
          gpu.submeshes[i].track_uv = is_track_texture_path(data.submeshes[i].base_map) ||
                                      data.submeshes[i].animated_uv;
          gpu.submeshes[i].track_uv_axis_v = data.submeshes[i].animated_uv_axis_v;
          fill_track_uv_strip(gpu.submeshes[i], data, data.submeshes[i]);
        }
      }
    } catch (const std::exception&) {
    }
    it = cache.emplace(vpath, std::move(gpu)).first;
  }
  return it->second;
}

struct Instance {
  std::string mesh_key;  // stable lookup into template_cache (never store raw map pointers)
  glm::mat4 model{1.0f};
  glm::vec3 origin{};
  std::uint32_t lightmap = 0;         // baked object lightmap atlas texture (0 = none)
  glm::vec4 lm_xform{1.f, 1.f, 0.f, 0.f};
  // 3 = RoadCompiled soft edge (vertex alpha × texture alpha → dirt blend).
  int alpha_mode = 0;
  // Local-space mesh radius — draw cull uses distance-to-sphere, not pivot alone.
  float cull_radius = 0.f;
};

// Extract a local-space triangle soup — delegated to collision_resolver (soldier
// then projectile cols; RoadCompiled / render-mesh fallback for decks).
std::vector<bf2::Float3> load_instance_collision(bf2::ResourceManager& resources,
                                                 const std::string& mesh_vpath) {
  return bf2::load_collision_soup(resources, mesh_vpath);
}

// RoadCompiled templates list textures in Roads/Splines/<name>_compiled.dat
// (one path per line). Returns up to two maps: base + overlay/detail.
std::pair<std::string, std::string> road_template_textures(bf2::ResourceManager& resources,
                                                          const std::string& template_name) {
  std::pair<std::string, std::string> out;
  if (template_name.empty()) return out;
  std::string key = template_name;
  for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const std::string candidates[] = {
      "roads/splines/" + key + "_compiled.dat",
      "roads/splines/" + key + ".dat",
  };
  for (const auto& path : candidates) {
    const auto bytes = resources.read_bytes(path);
    if (!bytes) continue;
    std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    std::istringstream in(text);
    std::string line;
    std::vector<std::string> maps;
    while (std::getline(in, line) && maps.size() < 2) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.pop_back();
      if (line.empty() || line[0] < 32) break;  // hit binary tail of .dat
      for (char& c : line) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (line.find("textures/") == std::string::npos && line.find(".dds") == std::string::npos) {
        // path without extension — texture_resolve adds .dds candidates
      }
      if (line.size() < 4 || line.compare(line.size() - 4, 4, ".dds") != 0) {
        line += ".dds";
      }
      maps.push_back(line);
    }
    if (!maps.empty()) {
      out.first = maps[0];
      if (maps.size() > 1) out.second = maps[1];
      // RoadCompiled.fx: detail0 = surface (CLAMP U), detail1 = tiling layer.
      // Decal/line overlays listed first would paint the whole road as streaks;
      // prefer the surface map as base and keep the line as detail.
      auto is_decal = [](const std::string& p) {
        return p.find("line") != std::string::npos || p.find("arrow") != std::string::npos ||
               p.find("manhole") != std::string::npos || p.find("overlay") != std::string::npos;
      };
      if (!out.second.empty() && is_decal(out.first) && !is_decal(out.second)) {
        std::swap(out.first, out.second);
      }
      // Identical pair: detail multiply at a single texel just darkens the road.
      if (out.first == out.second) out.second.clear();
      // Overlay-as-detail: BF2 lerps; our multiply turns overlays into stripes.
      if (!out.second.empty() && is_decal(out.second)) out.second.clear();
      return out;
    }
  }
  return out;
}

// Append an axis-aligned box (center c, half-extents h) with per-face normals to
// an interleaved position(3)+normal(3) vertex list and index list.
void add_box(std::vector<float>& v, std::vector<std::uint32_t>& idx, const glm::vec3& c,
             const glm::vec3& h) {
  const glm::vec3 faces[6][2] = {
      {{c.x + h.x, c.y, c.z}, {1, 0, 0}},  {{c.x - h.x, c.y, c.z}, {-1, 0, 0}},
      {{c.x, c.y + h.y, c.z}, {0, 1, 0}},  {{c.x, c.y - h.y, c.z}, {0, -1, 0}},
      {{c.x, c.y, c.z + h.z}, {0, 0, 1}},  {{c.x, c.y, c.z - h.z}, {0, 0, -1}},
  };
  for (int f = 0; f < 6; ++f) {
    const glm::vec3 n = faces[f][1];
    // Two in-plane axes for this face.
    glm::vec3 ua, ub;
    if (std::fabs(n.x) > 0.5f) {
      ua = {0, h.y, 0};
      ub = {0, 0, h.z};
    } else if (std::fabs(n.y) > 0.5f) {
      ua = {h.x, 0, 0};
      ub = {0, 0, h.z};
    } else {
      ua = {h.x, 0, 0};
      ub = {0, h.y, 0};
    }
    const glm::vec3 center = faces[f][0];
    const glm::vec3 corners[4] = {center - ua - ub, center + ua - ub, center + ua + ub,
                                  center - ua + ub};
    const std::uint32_t base = static_cast<std::uint32_t>(v.size() / 6);
    for (const auto& p : corners) {
      v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
    }
    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
}

// Build a simple first-person rifle in view space (looking down -Z).
void build_gun_mesh(std::vector<float>& v, std::vector<std::uint32_t>& idx) {
  const glm::vec3 gx(0.17f, -0.15f, 0.f);  // base offset (right/down)
  add_box(v, idx, gx + glm::vec3(0, 0.02f, -0.55f), {0.03f, 0.05f, 0.26f});    // receiver
  add_box(v, idx, gx + glm::vec3(0, 0.05f, -1.0f), {0.014f, 0.014f, 0.22f});   // barrel
  add_box(v, idx, gx + glm::vec3(0, -0.09f, -0.5f), {0.022f, 0.07f, 0.03f});   // magazine
  add_box(v, idx, gx + glm::vec3(0, -0.1f, -0.36f), {0.025f, 0.06f, 0.028f});  // grip
  add_box(v, idx, gx + glm::vec3(0, 0.02f, -0.26f), {0.028f, 0.045f, 0.07f});  // stock
  add_box(v, idx, gx + glm::vec3(0, 0.08f, -0.62f), {0.008f, 0.02f, 0.03f});   // rear sight
}

// Parse control-point spawn positions from a GamePlayObjects.con. Each control
// point is an "Object.create CPNAME_..." followed shortly by an
// "Object.absolutePosition x/y/z" -- those are valid on-land player spawns.
std::vector<glm::vec3> parse_control_points(const std::string& script) {
  std::vector<glm::vec3> out;
  std::istringstream lines(script);
  std::string line;
  std::string cur_name;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  while (std::getline(lines, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string lc = lower(cmd);
    if (lc == "object.create") {
      ls >> cur_name;
    } else if (lc == "object.absoluteposition" && !cur_name.empty()) {
      if (lower(cur_name).find("cpname") != std::string::npos) {
        std::string triple;
        ls >> triple;
        float v[3];
        if (bf2::detail::parse_triple(triple, v) == 3) {
          out.emplace_back(v[0], v[1], v[2]);
        }
      }
      cur_name.clear();
    }
  }
  return out;
}

// A vehicle placed by an ObjectSpawner: which vehicle template, and where.
struct VehicleSpawn {
  std::string vehicle;  // lower-case object-template name (e.g. "jeep_faav")
  glm::vec3 pos{};
  glm::vec3 rot{};  // yaw/pitch/roll degrees
};

// Parse GamePlayObjects.con: ObjectSpawner templates carry setObjectTemplate
// <team> <vehicle>, and their Object.create placements carry position/rotation.
std::vector<VehicleSpawn> parse_vehicle_spawns(const std::string& script) {
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  std::unordered_map<std::string, std::string> spawner_vehicle;  // spawner -> vehicle
  std::istringstream in(script);
  std::string line;
  std::string cur_tmpl;
  bool cur_is_spawner = false;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string lc = lower(cmd);
    if (lc == "objecttemplate.create") {
      std::string type, name;
      ls >> type >> name;
      cur_tmpl = lower(name);
      cur_is_spawner = lower(type) == "objectspawner";
    } else if (lc == "objecttemplate.setobjecttemplate" && cur_is_spawner && !cur_tmpl.empty()) {
      std::string team, veh;
      ls >> team >> veh;
      if (!veh.empty() && spawner_vehicle.find(cur_tmpl) == spawner_vehicle.end()) {
        spawner_vehicle[cur_tmpl] = lower(veh);
      }
    }
  }

  std::vector<VehicleSpawn> out;
  std::istringstream in2(script);
  std::string cur_obj;
  VehicleSpawn pending;
  bool have_pending = false;
  auto flush = [&]() {
    if (have_pending && !pending.vehicle.empty()) out.push_back(pending);
    have_pending = false;
  };
  while (std::getline(in2, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string lc = lower(cmd);
    if (lc == "object.create") {
      flush();
      std::string name;
      ls >> name;
      const auto it = spawner_vehicle.find(lower(name));
      if (it != spawner_vehicle.end()) {
        pending = VehicleSpawn{};
        pending.vehicle = it->second;
        have_pending = true;
      }
    } else if (have_pending && lc == "object.absoluteposition") {
      std::string triple;
      ls >> triple;
      float v[3];
      if (bf2::detail::parse_triple(triple, v) == 3) pending.pos = glm::vec3(v[0], v[1], v[2]);
    } else if (have_pending && lc == "object.rotation") {
      std::string triple;
      ls >> triple;
      float v[3];
      if (bf2::detail::parse_triple(triple, v) == 3) pending.rot = glm::vec3(v[0], v[1], v[2]);
    }
  }
  flush();
  return out;
}

std::string collision_path_for(const std::string& static_mesh_vpath) {
  return bf2::resolve_collision_vpath(static_mesh_vpath);
}

// A bone-tracked capsule collider. `a`/`b` are the world-space segment endpoints
// (a bone's two joints) and `r` its radius. `zone` selects the damage class.
struct Capsule {
  glm::vec3 a{};
  glm::vec3 b{};
  float r = 0.1f;
  int zone = 0;  // 0 = limb, 1 = torso, 2 = head
};

// Which skeleton joints form each capsule, its radius and its damage zone. Bone
// indices come from soldiers/common/animations/3p_setup.ske.
struct BonePair {
  int a, b;
  float r;
  int zone;
};
constexpr BonePair kBonePairs[] = {
    {46, 47, 0.14f, 2},   // neck -> head      : HEAD
    {13, 46, 0.19f, 1},   // torso -> neck     : upper chest
    {11, 13, 0.21f, 1},   // spine2 -> torso   : chest
    {0, 11, 0.19f, 1},    // root -> spine2    : pelvis/abdomen
    {1, 3, 0.12f, 0},     // L upper leg
    {6, 8, 0.12f, 0},     // R upper leg
    {3, 4, 0.09f, 0},     // L lower leg
    {8, 9, 0.09f, 0},     // R lower leg
    {15, 16, 0.09f, 0},   // L upper arm
    {31, 32, 0.09f, 0},   // R upper arm
    {16, 20, 0.07f, 0},   // L forearm
    {32, 35, 0.07f, 0},   // R forearm
};

// Closest squared distance between segments p1q1 and p2q2 (Ericson, RTCD). Also
// returns the parametric position `s` along the first segment at closest approach.
float closest_seg_seg_sq(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2,
                         const glm::vec3& q2, float& s) {
  const glm::vec3 d1 = q1 - p1;
  const glm::vec3 d2 = q2 - p2;
  const glm::vec3 r = p1 - p2;
  const float a = glm::dot(d1, d1);
  const float e = glm::dot(d2, d2);
  const float f = glm::dot(d2, r);
  float t;
  if (a <= 1e-8f && e <= 1e-8f) {
    s = 0.f;
    return glm::dot(r, r);
  }
  if (a <= 1e-8f) {
    s = 0.f;
    t = std::clamp(f / e, 0.f, 1.f);
  } else {
    const float c = glm::dot(d1, r);
    if (e <= 1e-8f) {
      t = 0.f;
      s = std::clamp(-c / a, 0.f, 1.f);
    } else {
      const float b = glm::dot(d1, d2);
      const float denom = a * e - b * b;
      s = denom > 1e-8f ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
      t = (b * s + f) / e;
      if (t < 0.f) {
        t = 0.f;
        s = std::clamp(-c / a, 0.f, 1.f);
      } else if (t > 1.f) {
        t = 1.f;
        s = std::clamp((b - c) / a, 0.f, 1.f);
      }
    }
  }
  const glm::vec3 c1 = p1 + d1 * s;
  const glm::vec3 c2 = p2 + d2 * t;
  return glm::dot(c1 - c2, c1 - c2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string archive_path;
  if (argc >= 2 && argv[1][0] != '-') archive_path = argv[1];

  // Optional headless screenshot: --shot <path.png> [frames] [drone]. Renders a
  // few frames (so terrain/objects/shadows settle) then writes a PNG and exits.
  std::string shot_path;
  int shot_frames = 30;
  bool shot_drone = false;
  bool shot_third_person = false;
  bool shot_missile = false;
  // Multiplayer: --host [port] / --dedicated [port] / --connect <addr[:port]>.
  int net_mode = 0;  // 0 off, 1 host, 2 connect
  bool net_dedicated = false;
  bool force_windowed_cli = false;
  std::uint16_t net_port = 27015;
  std::string net_connect;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
      shot_path = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-' && std::isdigit((unsigned char)argv[i + 1][0]) &&
          !std::filesystem::path(argv[i + 1]).has_extension()) {
        shot_frames = std::atoi(argv[++i]);
      }
    } else if (std::strcmp(argv[i], "--drone") == 0) {
      shot_drone = true;
    } else if (std::strcmp(argv[i], "--tp") == 0) {
      shot_third_person = true;
    } else if (std::strcmp(argv[i], "--missile") == 0) {
      shot_missile = true;
    } else if (std::strcmp(argv[i], "--host") == 0) {
      net_mode = 1;
      if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0])) net_port =
          static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--dedicated") == 0) {
      net_mode = 1;
      net_dedicated = true;
      if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0])) net_port =
          static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
      net_mode = 2;
      std::string a = argv[++i];
      const auto c = a.find(':');
      if (c != std::string::npos) {
        net_port = static_cast<std::uint16_t>(std::atoi(a.substr(c + 1).c_str()));
        a = a.substr(0, c);
      }
      net_connect = a;
    } else if (std::strcmp(argv[i], "--windowed") == 0) {
      force_windowed_cli = true;
    }
  }

  dalian::Settings settings = dalian::Settings::load();
  if (force_windowed_cli) {
    settings.fullscreen = dalian::FullscreenMode::Windowed;
    settings.width = 1920;
    settings.height = 1080;
  }
  const bool headless_shot = !shot_path.empty();
  if (headless_shot && archive_path.empty()) {
    std::cerr << "Usage: project_dalian <level_server.zip> --shot <path.png>\n";
    return 1;
  }

  // Window + GL (created before map selection so the menu can run).
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, settings.msaa > 0 ? 1 : 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, settings.msaa);

  SDL_Window* window = SDL_CreateWindow(
      "Project Dalian", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, settings.width,
      settings.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) {
    std::cerr << "glewInit failed\n";
    return 1;
  }
  dalian::apply_window_settings(window, settings);

  bf2::Renderer renderer;
  renderer.initialize(window);
  dalian::refresh_display(window, renderer, settings.width, settings.height);
  dalian::apply_graphics_settings(renderer, settings);
  renderer.reload_shadow_res(settings.shadow_res);

  dalian::resolve_bf2_root(settings, argc >= 2 ? argv[1] : nullptr);
  std::vector<dalian::MapEntry> map_list = dalian::scan_maps(settings.bf2_root);
  std::cout << "Found " << map_list.size() << " maps under "
            << (settings.bf2_root.empty() ? "?" : settings.bf2_root) << '\n';

  bool app_running = true;
  while (app_running) {
    dalian::MultiplayerConfig session_mp{};
    std::unique_ptr<bf2::Net> session_net;
    if (!headless_shot) {
      dalian::MenuResult mr = dalian::run_main_menu(window, renderer, settings, map_list);
      if (mr.action == dalian::MenuAction::Quit) break;
      if (mr.action != dalian::MenuAction::StartMap &&
          mr.action != dalian::MenuAction::StartMultiplayer)
        continue;
      archive_path = mr.map.server_zip;
      session_mp = mr.mp;
      session_net = std::move(mr.net);
    }

    const std::string bf2_root = settings.bf2_root;

    dalian::LoadingScreen loading_scr;
    if (!headless_shot) loading_scr.start_music(settings);

    auto load_step = [&](float p, const char* phase, const char* detail = nullptr) {
      if (!headless_shot) loading_scr.pump(window, renderer, p, phase, detail);
    };
    load_step(0.03f, "LOADING MAP", "Mounting level archives...");

    bool leave_to_menu = false;

    std::string objects_zip = (argc >= 3 && argv[2][0] != '-') ? argv[2] : std::string();
    if (objects_zip.empty()) {
      const auto p = std::filesystem::path(archive_path);
      if (p.has_parent_path() && p.parent_path().has_parent_path() &&
          p.parent_path().parent_path().has_parent_path()) {
        objects_zip = (p.parent_path().parent_path().parent_path() / "Objects_client.zip").string();
      }
    }

    bf2::ResourceManager resources;
    if (!resources.archives().mount(archive_path)) {
      std::cerr << "Failed to mount level archive: " << archive_path << '\n';
      if (headless_shot) return 1;
      archive_path.clear();
      continue;
    }
    load_step(0.10f, "LOADING MAP", "Mounting mod and retail BF2 archives...");

    const auto level_mod_dir = std::filesystem::path(archive_path).parent_path().parent_path().parent_path();
    const std::string level_mod_name = level_mod_dir.filename().string();
    auto mount_zip_if_exists = [&](const std::filesystem::path& dir, const char* name) -> bool {
      const auto p = dir / name;
      return std::filesystem::exists(p) && resources.archives().mount(p.string());
    };
    // Custom mods often ship maps without full Objects/Common archives. Mount retail
    // BF2 first so mod zips layered on top still override, but shared road textures
    // and props resolve from the base game.
    if (!bf2_root.empty() && level_mod_name != "bf2") {
      const auto retail = std::filesystem::path(bf2_root) / "mods" / "bf2";
      if (std::filesystem::is_directory(retail)) {
        int retail_mounted = 0;
        for (const char* z : {"Objects_client.zip", "Objects_server.zip", "Common_client.zip",
                              "Common_server.zip", "Booster_client.zip", "Booster_server.zip"}) {
          if (mount_zip_if_exists(retail, z)) ++retail_mounted;
        }
        if (retail_mounted > 0) {
          std::cout << "Retail BF2 archives: " << retail_mounted << " from " << retail.string()
                    << '\n';
        }
      }
    }

    const bool have_objects = !objects_zip.empty() && std::filesystem::exists(objects_zip) &&
                              resources.archives().mount(objects_zip);
    std::cout << "Objects archive: " << (have_objects ? objects_zip : "(none)") << '\n';

    // Objects_server.zip holds the vehicle .con/.tweak object hierarchy (child
    // meshes like rotors/turrets and their offsets). The client zip only has the
    // meshes/textures, so mount the server zip to assemble complete vehicles.
    if (!objects_zip.empty()) {
      auto server_zip = objects_zip;
      const auto pos = server_zip.rfind("Objects_client.zip");
      if (pos != std::string::npos) server_zip.replace(pos, 18, "Objects_server.zip");
      if (server_zip != objects_zip && std::filesystem::exists(server_zip) &&
          resources.archives().mount(server_zip)) {
        std::cout << "Objects server archive: " << server_zip << '\n';
      }
    }

    // Common_client.zip holds the shared terrain detail textures used by the
    // per-patch splat (Terrain/Textures/Detail/...).
    if (!objects_zip.empty()) {
      const auto common_zip =
          (std::filesystem::path(objects_zip).parent_path() / "Common_client.zip").string();
      if (std::filesystem::exists(common_zip) && resources.archives().mount(common_zip)) {
        std::cout << "Common archive: " << common_zip << '\n';
      }
      const auto mod_dir = std::filesystem::path(objects_zip).parent_path();
      for (const char* booster : {"Booster_client.zip", "Booster_server.zip"}) {
        const auto booster_zip = (mod_dir / booster).string();
        if (std::filesystem::exists(booster_zip) && resources.archives().mount(booster_zip)) {
          std::cout << "Booster archive: " << booster_zip << '\n';
        }
      }
    }

    const std::string client_zip = sibling_client_zip(archive_path);
    const bool have_client =
        !client_zip.empty() && resources.archives().mount(client_zip);
    std::cout << "Client archive: " << (have_client ? client_zip : "(none)") << '\n';

    bf2::LevelLoader loader(resources);
    const auto level = loader.load_mounted_level(archive_path);
    if (!level.has_terrain) {
      std::cerr << "No terrain in level\n";
      if (headless_shot) return 1;
      archive_path.clear();
      continue;
    }
    load_step(0.18f, "LOADING TERRAIN", "Heightmap, sky, atmosphere...");

    float xz = 2.0f;
    std::string heightdata_con;
    if (const auto hd = resources.read_bytes("Heightdata.con")) {
      heightdata_con.assign(reinterpret_cast<const char*>(hd->data()), hd->size());
      xz = bf2::parse_heightmap_xz_scale(heightdata_con, xz);
    }

    // Sky / water / sun settings for the gradient sky, fog and water plane.
    std::string water_con, sky_con;
    if (const auto b = resources.read_bytes("Water.con")) {
      water_con.assign(reinterpret_cast<const char*>(b->data()), b->size());
    }
    if (const auto b = resources.read_bytes("Sky.con")) {
      sky_con.assign(reinterpret_cast<const char*>(b->data()), b->size());
    }
    bf2::Atmosphere atmo = bf2::parse_atmosphere(water_con, sky_con, heightdata_con);
    {
      const float fog_scale = settings.fog_scale;
      // Remastered visibility: fog must not erase geometry before draw distance.
      const float clear_end = std::max(8000.f, settings.draw_distance * 1.15f);
      const float clear_start = std::min(std::max(atmo.fog_start, 1800.f), clear_end * 0.45f);
      atmo.fog_start = clear_start * fog_scale;
      atmo.fog_end = clear_end * fog_scale;
    }
    std::cout << "Atmosphere: sky " << atmo.sky_color.x << "/" << atmo.sky_color.y << "/"
              << atmo.sky_color.z << (atmo.is_night ? " (night)" : "")
              << (atmo.has_moon ? " moon" : "")
              << (atmo.has_water ? "  water level " : "  (no water")
              << (atmo.has_water ? std::to_string(atmo.water_level) : std::string(")"))
              << "  fog " << atmo.fog_start << ".." << atmo.fog_end;
    if (!atmo.sky_texture.empty()) std::cout << "  skydome";
    if (atmo.has_cloud_layer) std::cout << "  clouds";
    std::cout << '\n';
    load_step(0.20f, "LOADING TERRAIN", "Navigation mesh...");

    bf2::NavMesh nav_mesh;
    if (const auto nav_bytes = resources.read_bytes("GTSData/output/Infantry.vbf")) {
      if (nav_mesh.load_from_bytes(*nav_bytes)) {
        std::cout << "Nav mesh: " << nav_mesh.vertex_count() << " verts, "
                  << nav_mesh.triangle_count() << " tris\n";
      }
    }

    bf2::TemplateResolver resolver(resources);
    std::string static_script, dummy_script;
    if (const auto so = resources.read_bytes("StaticObjects.con")) {
      static_script.assign(reinterpret_cast<const char*>(so->data()), so->size());
    }
    if (const auto dummy = resources.read_bytes("DummyObjects.con")) {
      dummy_script.assign(reinterpret_cast<const char*>(dummy->data()), dummy->size());
    }
    resolver.build_from_level_scripts(static_script, dummy_script);
    std::cout << "Resolved " << resolver.map().size() << " templates; " << level.placements.size()
              << " placements\n";
    load_step(0.23f, "LOADING TERRAIN", "Auditing templates and textures...");

    {
      std::vector<std::string> tmpl_names;
      tmpl_names.reserve(level.placements.size());
      for (const auto& p : level.placements) tmpl_names.push_back(p.template_name);
      const auto audit = bf2::audit_static_assets(resources, resolver, tmpl_names);
      bf2::log_asset_audit(audit, std::getenv("BF2_TEXAUDIT") != nullptr);
    }

    bf2::TextureCache textures(resources, renderer);
    const std::uint32_t fx_smoke_tex =
        textures.get("effects/textures/animated/anim_smoketrail.dds");
    const std::uint32_t fx_fire_tex =
        textures.get("effects/textures/fire/tank_muzzleflash.dds");
    const std::uint32_t fx_flash_tex =
        textures.get("effects/textures/singles/flash.dds");
    if (fx_smoke_tex) std::cout << "FX: smoke/trail texture loaded\n";
    if (fx_fire_tex) std::cout << "FX: fire/explosion texture loaded\n";
    if (fx_flash_tex) std::cout << "FX: afterburner flash texture loaded\n";

    std::uint32_t cloud_tex = 0;
    std::uint32_t sky_tex = 0;
    if (!atmo.sky_texture.empty()) {
      sky_tex = textures.get(atmo.sky_texture + ".dds");
      if (!sky_tex) sky_tex = textures.get(atmo.sky_texture);
      if (sky_tex) std::cout << "Skydome texture loaded: " << atmo.sky_texture << '\n';
      else std::cout << "Skydome texture missing: " << atmo.sky_texture << '\n';
    }
    if (!atmo.cloud_texture.empty()) {
      cloud_tex = textures.get(atmo.cloud_texture + ".dds");
      if (!cloud_tex) cloud_tex = textures.get(atmo.cloud_texture);
      if (cloud_tex) std::cout << "Sky cloud texture loaded\n";
    }

    bf2::EffectBundleLibrary fx_library;
    for (const char* bundle :
         {"e_jetexhaust_AB", "e_muzz_rocketpod", "e_muzz_ak101", "e_muzz_tank",
          "e_missile_trail", "e_missile_ignition", "e_vexp_igla",
          "e_mexp_grenade_grass", "e_mexp_grenade_dirt", "e_mexp_grenade_water",
          "e_mexp_grenade_sand", "e_mexp_grenade_rock", "e_vexp_tank", "e_vexp_heli_generic",
          "e_sAmb_Fountain_waterSpray", "e_sAmb_OnlySmoke", "e_sAmb_fire"}) {
      if (fx_library.load(resources, bundle)) {
        std::cout << "FX bundle loaded: " << bundle << '\n';
      }
    }
    dalian::init_bf2_fx(&fx_library);
    load_step(0.27f, "LOADING TERRAIN", "Terrain colormap and foliage...");

    // Terrain: colormap + lightmap tiles from client.zip when available.
  bf2::TerrainVisualConfig terrain_cfg;
  bf2::TerrainGroundTextures ground_tex{};
  bool have_ground_tex = false;
  if (have_client) {
    if (const auto tc = resources.read_bytes("Terrain.con")) {
      const std::string script(reinterpret_cast<const char*>(tc->data()), tc->size());
      terrain_cfg = bf2::TerrainColormapLoader::parse_terrain_con(script);
    }
    const auto atlases = bf2::TerrainColormapLoader::build_atlases(resources, terrain_cfg);
    if (!atlases.colormap.pixels.empty()) {
      ground_tex = bf2::TerrainColormapLoader::upload(atlases);
    }
    have_ground_tex = ground_tex.colormap != 0;
    if (have_ground_tex) {
      std::cout << "Terrain colormap " << ground_tex.tile_cols << "x" << ground_tex.tile_rows
                << " tiles" << (ground_tex.lightmap != 0 ? " + lightmaps" : "")
                << (ground_tex.splat ? " + detail splat" : "")
                << (ground_tex.mask1 != 0 ? " (mask1)" : "")
                << (ground_tex.mask2 != 0 ? " (mask2)" : "") << '\n';
    } else {
      std::cout << "Terrain colormap not loaded (missing client tiles?)\n";
    }
  }

  bf2::OvergrowthParser overgrowth_defs;
  if (const auto og = resources.read_bytes("Overgrowth/Overgrowth.con")) {
    const std::string script(reinterpret_cast<const char*>(og->data()), og->size());
    overgrowth_defs.parse(script);
    std::cout << "Overgrowth: " << overgrowth_defs.materials().size() << " materials, "
              << overgrowth_defs.types().size() << " types (instances in Overgrowth.raw)\n";
  }

  bf2::GpuMesh terrain_gpu{};
  bf2::GpuTexturedMesh terrain_tex_gpu{};
  if (have_ground_tex) {
    const auto terrain_data = bf2::terrain_to_textured_mesh(level.terrain, xz, 2);
    terrain_tex_gpu = renderer.upload_textured(terrain_data);
  } else {
    auto terrain_mesh_cpu = terrain_to_mesh(level.terrain, xz, 2);
    terrain_gpu = renderer.upload_mesh(terrain_mesh_cpu);
  }

  // Baked per-instance object lightmaps (Lightmaps/Objects/): index + atlases.
  bf2::ObjectLightmaps obj_lm;
  std::vector<std::uint32_t> lm_atlases;
  if (have_client) {
    if (const auto b = resources.read_bytes("Lightmaps/Objects/LightmapAtlas.tai")) {
      obj_lm = bf2::parse_object_lightmaps(
          std::string(reinterpret_cast<const char*>(b->data()), b->size()));
      lm_atlases.assign(static_cast<std::size_t>(obj_lm.atlas_count), 0u);
      for (int i = 0; i < obj_lm.atlas_count; ++i) {
        char path[128];
        std::snprintf(path, sizeof(path), "Lightmaps/Objects/LightmapAtlas%d.dds", i);
        try {
          const auto tex = resources.load_texture(path);
          lm_atlases[static_cast<std::size_t>(i)] = renderer.upload_texture(tex);
        } catch (const std::exception&) {
        }
      }
      std::cout << "Object lightmaps: " << obj_lm.by_name.size() << " meshes, " << obj_lm.atlas_count
                << " atlases\n";
    }
  }
  auto basename_lower = [](const std::string& p) {
    return bf2::detail::basename_no_ext(p);
  };

  // Undergrowth (grass): material map + config + atlas, scattered around the
  // camera each frame the player moves.
  bf2::Undergrowth undergrowth;
  std::uint32_t grass_atlas_tex = 0;
  if (have_client) {
    const auto cfg = resources.read_bytes("Undergrowth.cfg");
    const auto raw = resources.read_bytes("Undergrowth.raw");
    const auto tai = resources.read_bytes("UndergrowthAtlas.tai");
    if (cfg && raw && tai) {
      // Terrain cell size from Terrain.con primaryWorldScale (X), default 2m.
      float cell = 2.0f;
      if (const auto tc = resources.read_bytes("Terrain.con")) {
        const std::string s(reinterpret_cast<const char*>(tc->data()), tc->size());
        const auto p = s.find("primaryWorldScale");
        if (p != std::string::npos) {
          float v = 0.f;
          if (std::sscanf(s.c_str() + p, "primaryWorldScale %f", &v) == 1 && v > 0.f) cell = v;
        }
      }
      undergrowth = bf2::parse_undergrowth(
          std::string(reinterpret_cast<const char*>(cfg->data()), cfg->size()), *raw,
          std::string(reinterpret_cast<const char*>(tai->data()), tai->size()), cell);
      try {
        // No mipmaps: mip-averaging this alpha-tested atlas melts distant blades
        // (plus their white background) into opaque grey squares.
        const auto atlas_dds = resources.load_texture("UndergrowthAtlas0.dds");
        grass_atlas_tex = renderer.upload_texture(atlas_dds, false);
        // Prune grass whose atlas cell is a solid opaque block (reserved/padding
        // slots in the atlas). A real blade cell is mostly transparent; a solid
        // block would render as grey squares on the ground. Map-agnostic.
        const auto rgba = bf2::DdsLoader::decode_to_rgba8(atlas_dds);
        const int aw = static_cast<int>(rgba.width), ah = static_cast<int>(rgba.height);
        if (aw > 0 && ah > 0) {
          for (auto it = undergrowth.grass.begin(); it != undergrowth.grass.end();) {
            const glm::vec4 r = it->second.atlas_rect;
            const int x0 = std::clamp(static_cast<int>(r.x * aw), 0, aw - 1);
            const int x1 = std::clamp(static_cast<int>((r.x + r.z) * aw), x0 + 1, aw);
            const int y0 = std::clamp(static_cast<int>(r.y * ah), 0, ah - 1);
            const int y1 = std::clamp(static_cast<int>((r.y + r.w) * ah), y0 + 1, ah);
            auto op = [&](int x, int y) {
              return rgba.pixels[(static_cast<std::size_t>(y) * aw + x) * 4 + 3] > 128;
            };
            std::size_t opaque = 0, total = 0, interior = 0;
            for (int y = y0; y < y1; ++y) {
              for (int x = x0; x < x1; ++x) {
                ++total;
                if (!op(x, y)) continue;
                ++opaque;
                // "Interior" = an opaque texel whose 4 neighbours are also opaque.
                // Thin grass blades (1-3px wide) have almost none; a solid packed
                // block/sprite has many, so it flags atlas contamination.
                if (x > x0 && x < x1 - 1 && y > y0 && y < y1 - 1 && op(x - 1, y) && op(x + 1, y) &&
                    op(x, y - 1) && op(x, y + 1)) {
                  ++interior;
                }
              }
            }
            const double cov = total ? static_cast<double>(opaque) / total : 0.0;
            const double solid = total ? static_cast<double>(interior) / total : 0.0;
            // Prune cells that aren't thin-bladed grass: fully opaque/empty, or
            // containing a solid filled region (packed sprite or atlas padding
            // that would render as a grey square on the ground).
            if (cov > 0.88 || cov < 0.003 || solid > 0.035) {
              it = undergrowth.grass.erase(it);
            } else {
              ++it;
            }
          }
        }
      } catch (const std::exception&) {
      }
      std::cout << "Undergrowth: " << undergrowth.width << "x" << undergrowth.height << " map, "
                << undergrowth.grass.size() << " grass materials, cell " << cell << "m\n";
    }
  }

  // Physics world (terrain sampling for foliage snap + collision build below).
  bf2::PhysicsWorld world;
  world.set_terrain(level.terrain, xz, /*centered=*/true);
  if (level.has_heightmap_cluster) {
    world.set_heightmap_cluster(&level.heightmap_cluster);
  }

  // Build one textured GPU mesh per unique resolved template.
  std::unordered_map<std::string, bf2::GpuTexturedMesh> template_cache;
  template_cache.reserve(512);
  std::unordered_map<std::string, bf2::MeshLocalBounds> mesh_bounds_cache;
  // Local-space collision triangle soup per unique template.
  std::unordered_map<std::string, std::vector<bf2::Float3>> collision_cache;
  collision_cache.reserve(512);
  std::vector<Instance> instances;
  instances.reserve(level.placements.size());
  int resolved = 0;
  int collision_tris = 0;
  int collision_miss = 0;
  const std::size_t placement_total = level.placements.size();
  std::size_t placement_i = 0;
  for (const auto& inst : level.placements) {
    if ((placement_i & 7u) == 0u || placement_i + 1 == placement_total) {
      const float t =
          placement_total > 0 ? static_cast<float>(placement_i) / static_cast<float>(placement_total)
                              : 1.f;
      load_step(0.32f + 0.08f * t, "LOADING WORLD", "Uploading static meshes...");
    }
    ++placement_i;
    const std::string vpath = resolver.resolve_mesh(inst.template_name);
    if (vpath.empty()) {
      continue;
    }
    auto& gpu = ensure_textured_mesh(vpath, template_cache, resources, textures, renderer);
    if (collision_cache.find(vpath) == collision_cache.end()) {
      collision_cache.emplace(vpath, load_instance_collision(resources, vpath));
    }
    if (gpu.vao != 0) {
      bf2::ObjectInstance placed = inst;
      const bf2::MeshLocalBounds bounds =
          bf2::mesh_local_bounds(resources, vpath, &mesh_bounds_cache);
      const float min_y = bounds.min_y;
      // Prefer BF2 authored Y. Only correct small float/embed gaps vs terrain so
      // rooftop/bridge props (large gaps) stay put. Foliage gets a slightly larger
      // float allowance because editor trees are often a bit proud of the mesh.
      const float max_float =
          bf2::is_foliage_template(inst.template_name) ? 2.5f : 1.25f;
      const auto snap =
          bf2::snap_placement_y(world, placed.position[0], placed.position[1], placed.position[2],
                                min_y, /*max_pull_down=*/0.85f, max_float);
      if (snap.snapped) placed.position[1] = snap.y;
      Instance in;
      in.mesh_key = vpath;
      in.model = placement_matrix(placed);
      in.origin = glm::vec3(placed.position[0], placed.position[1], placed.position[2]);
      in.cull_radius = bounds.cull_radius;
      if (!lm_atlases.empty()) {
        if (const auto* e = obj_lm.find(basename_lower(vpath), in.origin)) {
          if (e->atlas >= 0 && e->atlas < static_cast<int>(lm_atlases.size())) {
            in.lightmap = lm_atlases[static_cast<std::size_t>(e->atlas)];
            in.lm_xform = e->xform;
          }
        }
      }
      instances.push_back(in);
      ++resolved;
      const auto& soup = collision_cache[vpath];
      if (soup.empty()) {
        ++collision_miss;
      } else {
        collision_tris += static_cast<int>(bf2::count_collision_soup_tris(soup));
      }
    }
  }

  // Compiled road meshes from CompiledRoads.con (client.zip).
  // These are RoadCompiled binaries (not StaticMesh) — dedicated loader.
  int road_resolved = 0;
  int road_tex_miss = 0;
  const std::size_t road_total = level.roads.size();
  std::size_t road_i = 0;
  for (const auto& road : level.roads) {
    if ((road_i & 3u) == 0u || road_i + 1 == road_total) {
      const float t =
          road_total > 0 ? static_cast<float>(road_i) / static_cast<float>(road_total) : 1.f;
      load_step(0.40f + 0.02f * t, "LOADING WORLD", "Compiled roads...");
    }
    ++road_i;
    if (road.mesh_vpath.empty()) continue;
    const std::string resolved_mesh =
        bf2::resolve_mesh_vpath(resources.archives(), road.mesh_vpath);
    if (collision_cache.find(resolved_mesh) == collision_cache.end()) {
      collision_cache.emplace(resolved_mesh,
                              load_instance_collision(resources, resolved_mesh));
    }

    if (template_cache.find(resolved_mesh) == template_cache.end()) {
      bf2::GpuTexturedMesh gpu{};
      try {
        const auto bytes = resources.read_bytes(resolved_mesh);
        if (bytes && bf2::is_road_compiled_bytes(*bytes)) {
          const auto road_mesh = bf2::load_road_compiled(*bytes);
          const auto maps = road_template_textures(resources, road.template_name);
          const auto data =
              bf2::road_compiled_to_textured(road_mesh, maps.first, maps.second);
          if (!data.vertices.empty()) {
            gpu = renderer.upload_textured(data);
            const std::string tex_folder = mesh_texture_folder(resolved_mesh);
            for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
              gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map, tex_folder);
              gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map, tex_folder);
            }
          }
        }
      } catch (const std::exception&) {
      }
      template_cache.emplace(resolved_mesh, std::move(gpu));
    }

    auto& gpu = template_cache[resolved_mesh];
    if (gpu.vao == 0) continue;
    for (const auto& sub : gpu.submeshes) {
      if (sub.base_tex == 0) ++road_tex_miss;
    }
    Instance in;
    in.mesh_key = resolved_mesh;
    // RoadCompiled verts are local to `start`, which matches absoluteposition.
    // Extra 5cm lift stacks with the mesh-local lift to beat terrain z-fight.
    in.model = glm::translate(
        glm::mat4(1.f),
        glm::vec3(road.position[0], road.position[1] + 0.05f, road.position[2]));
    in.origin = glm::vec3(road.position[0], road.position[1], road.position[2]);
    in.alpha_mode = 3;  // soft edge fade into terrain (BF2 RoadCompiled Alpha)
    instances.push_back(in);
    ++road_resolved;
    const auto& soup = collision_cache[resolved_mesh];
    if (soup.empty()) {
      ++collision_miss;
    } else {
      collision_tris += static_cast<int>(bf2::count_collision_soup_tris(soup));
    }
  }
  if (road_resolved > 0) {
    std::cout << "CompiledRoads: rendered " << road_resolved << " segments";
    if (road_tex_miss > 0) std::cout << " (" << road_tex_miss << " submeshes missing road textures)";
    std::cout << '\n';
  }

  std::cout << "Uploaded " << template_cache.size() << " unique meshes, " << resolved
            << " static instances; textures loaded " << textures.loaded_count() << ", missing "
            << textures.missing_count() << "; collision tris " << collision_tris << ", meshes w/o "
            << "collision " << collision_miss << '\n';
  load_step(0.42f, "BUILDING WORLD", "Trees, collision, and spawn setup...");

  std::vector<dalian::AmbientEmitter> ambient_emitters =
      dalian::collect_ambient_emitters(level.placements);
  const std::vector<dalian::SceneLight> scene_lights =
      dalian::collect_scene_lights(ambient_emitters);
  if (!ambient_emitters.empty()) {
    std::cout << "Ambient emitters: " << ambient_emitters.size() << " placements";
    if (!scene_lights.empty()) std::cout << " (" << scene_lights.size() << " baselights)";
    std::cout << "\n";
  }

  // Physics / player (world already has terrain + heightmap cluster).
  if (level.has_heightmap_cluster) {
    std::cout << "Physics: using full 3x3 heightmap cluster (" << level.heightmap_cluster.patches().size()
              << " patches)\n";
  }

  // Overgrowth trees/bushes from Overgrowth.raw material grid.
  int tree_resolved = 0;
  if (!overgrowth_defs.types().empty()) {
    if (const auto og_raw = resources.read_bytes("Overgrowth/Overgrowth.raw")) {
      auto trees = bf2::build_overgrowth_instances(overgrowth_defs, *og_raw, resources, xz);
      const std::size_t tree_total = trees.size();
      for (std::size_t ti = 0; ti < trees.size(); ++ti) {
        if ((ti & 15u) == 0u || ti + 1 == tree_total) {
          const float t =
              tree_total > 0 ? static_cast<float>(ti) / static_cast<float>(tree_total) : 1.f;
          load_step(0.43f + 0.04f * t, "BUILDING WORLD", "Trees and bushes...");
        }
        const auto& tr = trees[ti];
        auto& gpu = ensure_textured_mesh(tr.mesh_vpath, template_cache, resources, textures, renderer);
        if (gpu.vao == 0) continue;
        bf2::ObjectInstance pseudo;
        pseudo.position[0] = tr.position[0];
        const bf2::MeshLocalBounds bounds =
            bf2::mesh_local_bounds(resources, tr.mesh_vpath, &mesh_bounds_cache);
        const float min_y = bounds.min_y;
        pseudo.position[1] =
            world.terrain_height(tr.position[0], tr.position[2]) - min_y * tr.scale;
        pseudo.position[2] = tr.position[2];
        pseudo.rotation[0] = tr.rotation[0];
        pseudo.rotation[1] = tr.rotation[1];
        pseudo.rotation[2] = tr.rotation[2];
        Instance in;
        in.mesh_key = tr.mesh_vpath;
        in.model = placement_matrix(pseudo);
        if (tr.scale != 1.f) {
          in.model = glm::scale(in.model, glm::vec3(tr.scale));
        }
        in.origin = glm::vec3(pseudo.position[0], pseudo.position[1], pseudo.position[2]);
        in.cull_radius = bounds.cull_radius * tr.scale;
        instances.push_back(in);
        ++tree_resolved;
      }
      std::cout << "Overgrowth: instanced " << tree_resolved << " trees/bushes\n";
    }
  }

  // Feed world-space collision triangles for every placed building.
  const std::size_t inst_total = instances.size();
  for (std::size_t ii = 0; ii < instances.size(); ++ii) {
    if ((ii & 15u) == 0u || ii + 1 == inst_total) {
      const float t =
          inst_total > 0 ? static_cast<float>(ii) / static_cast<float>(inst_total) : 1.f;
      load_step(0.48f + 0.08f * t, "BUILDING WORLD", "Baking collision...");
    }
    const auto& in = instances[ii];
    const auto cit = collision_cache.find(in.mesh_key);
    if (cit == collision_cache.end() || cit->second.empty()) {
      continue;
    }
    const auto& soup = cit->second;
    for (std::size_t i = 0; i + 2 < soup.size(); i += 3) {
      bf2::Float3 w[3];
      for (int k = 0; k < 3; ++k) {
        const glm::vec4 p =
            in.model * glm::vec4(soup[i + k].x, soup[i + k].y, soup[i + k].z, 1.0f);
        w[k] = {p.x, p.y, p.z};
      }
      world.add_collision_triangle(w[0], w[1], w[2]);
    }
  }
  world.finalize_colliders();
  std::cout << "Collision triangles: " << world.collision_triangle_count() << '\n';

  dalian::GameSim game_sim;
  bf2::CharacterController& player = game_sim.state().player;
  player.eye_height = 1.8f;

  // Spawn at the original BF2 spawn: the control point nearest the map centre
  // that sits above the water line. Falls back to map centre when no gameplay
  // layout is found.
  glm::vec3 spawn(0.f, world.terrain_height(0.f, 0.f) + player.eye_height + 2.f, 0.f);
  std::string gameplay_script;   // layer used for control-point/player spawns
  std::string vehicle_script;    // layer used for vehicle placement (richest set)
  std::vector<glm::vec3> control_points;  // above-water objectives, for enemy placement
  dalian::MapConquestLayout map_layout;
  {
    // Larger conquest layers (64/32) carry the full vehicle roster including
    // jets and helicopters; the 16-player COOP layer often omits aircraft. We
    // therefore pick control points from the first layer that has them, but
    // source vehicles from whichever layer spawns the most vehicles.
    std::vector<glm::vec3> cps;
    std::size_t best_veh = 0;
    for (const char* gm : {"GameModes/gpm_cq/64/GamePlayObjects.con",
                           "GameModes/gpm_cq/32/GamePlayObjects.con",
                           "GameModes/gpm_coop/64/GamePlayObjects.con",
                           "GameModes/gpm_coop/32/GamePlayObjects.con",
                           "GameModes/gpm_coop/16/GamePlayObjects.con",
                           "GameModes/gpm_cq/16/GamePlayObjects.con",
                           "GameModes/sp1/16/GamePlayObjects.con"}) {
      const auto b = resources.read_bytes(gm);
      if (!b) continue;
      std::string script(reinterpret_cast<const char*>(b->data()), b->size());
      if (cps.empty()) {
        auto layer_cps = parse_control_points(script);
        if (!layer_cps.empty()) {
          cps = std::move(layer_cps);
          gameplay_script = script;
          std::cout << "Spawns: " << cps.size() << " control points from " << gm << '\n';
        }
      }
      const std::size_t nveh = parse_vehicle_spawns(script).size();
      if (nveh > best_veh) {
        best_veh = nveh;
        vehicle_script = std::move(script);
        std::cout << "Vehicles: layer " << gm << " has " << nveh << " spawns\n";
      }
    }
    if (vehicle_script.empty()) vehicle_script = gameplay_script;
    const float water = atmo.has_water ? atmo.water_level : -1e9f;
    float best_d2 = std::numeric_limits<float>::max();
    for (const auto& cp : cps) {
      if (cp.y <= water + 0.5f) continue;  // skip anything at/under the sea
      control_points.push_back(cp);
      const float d2 = cp.x * cp.x + cp.z * cp.z;
      if (d2 < best_d2) {
        best_d2 = d2;
        spawn = glm::vec3(cp.x, cp.y + player.eye_height + 1.0f, cp.z);
      }
    }
    if (!gameplay_script.empty()) map_layout = dalian::parse_map_conquest(gameplay_script);
  }
  player.position = {spawn.x, spawn.y, spawn.z};
  std::cout << "Spawn @ " << spawn.x << " " << spawn.y << " " << spawn.z << '\n';

  // Vehicles: placed by ObjectSpawners in the gameplay layout. A BF2 vehicle is
  // assembled from a root bundledmesh (its 3rd-person "geom1" external body) plus
  // separate child meshes (rotors, sometimes turrets) attached at offsets defined
  // in the vehicle's .tweak/.con. Parts internal to the body (wings, gun turret,
  // landing skids) are already baked into the body mesh as geometry parts.
  using VehiclePart = dalian::VehiclePart;
  using VehicleWheelSlot = dalian::VehicleWheelSlot;
  using Vehicle = dalian::Vehicle;
  using Enemy = dalian::Enemy;
  using ActiveMissile = dalian::ActiveMissile;
  using Tracer = dalian::Tracer;
  using Impact = dalian::Impact;
  using Projectile = dalian::Projectile;
  using Smoke = dalian::Smoke;
  using Explosion = dalian::Explosion;
  using Flare = dalian::Flare;
  std::vector<Vehicle> loaded_vehicles;
  std::unordered_map<std::string, bf2::GpuTexturedMesh> vehicle_cache;
  // Lowest assembled-mesh Y per vehicle body path (relative to its origin).
  std::unordered_map<std::string, float> vehicle_min_y;
  // Local collision half-extents (x=right, y=height, z=forward) per body path.
  std::unordered_map<std::string, glm::vec3> vehicle_half_xz;
  if (!gameplay_script.empty()) {
    // Index every .bundledmesh by base name so vehicle templates resolve to a mesh.
    std::unordered_map<std::string, std::string> bundled_by_name;
    for (const auto& p : resources.archives().list()) {
      if (p.size() > 12 && p.compare(p.size() - 12, 12, ".bundledmesh") == 0) {
        bundled_by_name.emplace(bf2::detail::basename_no_ext(p), p);
      }
    }
    // BF2 vehicle geometry convention: geom0 = 1P (cockpit/driver view),
    // geom1 = 3P external body, geom2 = wreck. Always use the external body when
    // present (picking "most triangles" wrongly selected detailed cockpits).
    auto external_geometry = [](const bf2::Mesh& mesh) -> std::size_t {
      if (mesh.geometries.size() >= 2 && !mesh.geometries[1].lods.empty() &&
          !mesh.geometries[1].lods[0].materials.empty()) {
        return 1;
      }
      return 0;
    };
    struct PartHierarchy {
      std::vector<glm::mat4> xforms;
      std::vector<dalian::VehicleWheelSlot> wheels;
      std::vector<dalian::VehicleGearSlot> gear_parts;
    };
    // Load a vehicle/part mesh (external geom, LOD0) into the shared cache. When
    // `part_xform` is supplied (the body mesh), each vertex is moved from its
    // part-local space into assembled vehicle space using its geometry-part index,
    // so turrets, gun barrels and wheels sit where they belong instead of
    // collapsing onto the hull at the origin.
    auto load_vehicle_mesh = [&](const std::string& vpath, const PartHierarchy* hierarchy = nullptr,
                                 int geom_override = -1,
                                 const std::string& cache_key_in = "") -> bool {
      const std::string cache_key = cache_key_in.empty() ? vpath : cache_key_in;
      if (auto it = vehicle_cache.find(cache_key); it != vehicle_cache.end()) {
        return it->second.vao != 0;
      }
      auto upload_mesh_data = [&](bf2::TexturedMeshData& data, const std::string& key,
                                  const std::string& log_vpath,
                                  float min_y_override = std::numeric_limits<float>::quiet_NaN()) -> bool {
        if (data.vertices.empty()) return false;
        if (std::isnan(min_y_override)) {
          std::vector<float> ys;
          ys.reserve(data.vertices.size());
          for (const auto& vtx : data.vertices) ys.push_back(vtx.position.y);
          const std::size_t k =
              std::min(ys.size() - 1, static_cast<std::size_t>(ys.size() * 0.015f));
          std::nth_element(ys.begin(), ys.begin() + k, ys.end());
          vehicle_min_y[key] = ys[k];
        } else {
          vehicle_min_y[key] = min_y_override;
        }
        if (key == vpath) {
          std::vector<float> xs, zs, hs;
          xs.reserve(data.vertices.size());
          zs.reserve(data.vertices.size());
          hs.reserve(data.vertices.size());
          for (const auto& vtx : data.vertices) {
            xs.push_back(std::fabs(vtx.position.x));
            zs.push_back(std::fabs(vtx.position.z));
            hs.push_back(vtx.position.y);
          }
          auto pct = [](std::vector<float>& v, float p) {
            const std::size_t idx =
                std::min(v.size() - 1, static_cast<std::size_t>(v.size() * p));
            std::nth_element(v.begin(), v.begin() + idx, v.end());
            return v[idx];
          };
          const float hx = pct(xs, 0.985f);
          const float hz = pct(zs, 0.985f);
          const float top = pct(hs, 0.985f);
          vehicle_half_xz[key] = glm::vec3(hx, top - vehicle_min_y[key], hz);
        }
        if (data.submeshes.empty() && !data.indices.empty()) {
          bf2::TexturedSubmesh sm;
          sm.index_offset = 0;
          sm.index_count = static_cast<std::uint32_t>(data.indices.size());
          data.submeshes.push_back(sm);
        }
        bf2::GpuTexturedMesh gpu = renderer.upload_textured(data);
        const std::string tex_folder = mesh_texture_folder(log_vpath);
        int missing = 0;
        for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
          gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map, tex_folder);
          gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map, tex_folder);
          gpu.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map, tex_folder);
          gpu.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map, tex_folder);
          gpu.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map, tex_folder);
          gpu.submeshes[i].specular_tex =
              textures.get(data.submeshes[i].specular_map, tex_folder);
          // Do NOT set cutout here. BF2 vehicle/kit maps often put specular in
          // alpha; the foliage cutout heuristic would discard most of the hull.
          if (gpu.submeshes[i].base_tex == 0) ++missing;
          gpu.submeshes[i].track_uv = is_track_texture_path(data.submeshes[i].base_map) ||
                                      data.submeshes[i].animated_uv;
          gpu.submeshes[i].track_uv_axis_v = data.submeshes[i].animated_uv_axis_v;
          fill_track_uv_strip(gpu.submeshes[i], data, data.submeshes[i]);
        }
        if (missing > 0 && std::getenv("BF2_VEHLIST"))
          std::cerr << "  TEXMISS " << bf2::detail::basename_no_ext(log_vpath) << ": " << missing
                    << "/" << gpu.submeshes.size() << " submeshes missing base texture\n";
        const bool ok = gpu.vao != 0;
        vehicle_cache[key] = std::move(gpu);
        return ok;
      };
      // Wheels/gear are separate geometry parts with their own rest pose; clearance
      // must use the lowest vertex in assembled vehicle space, not hull-only bounds.
      auto transformed_mesh_low_y = [](const bf2::TexturedMeshData& data,
                                       const glm::mat4& xf) -> float {
        if (data.vertices.empty()) return 0.f;
        float low = std::numeric_limits<float>::max();
        for (const auto& vtx : data.vertices) {
          const glm::vec4 wp =
              xf * glm::vec4(vtx.position.x, vtx.position.y, vtx.position.z, 1.f);
          low = std::min(low, wp.y);
        }
        return low;
      };

      bf2::GpuTexturedMesh gpu;
      try {
        const auto mesh = resources.load_mesh(vpath);
        const std::size_t geom =
            geom_override >= 0 ? static_cast<std::size_t>(geom_override) : external_geometry(mesh);
        auto data = bf2::MeshLoader::extract_textured(mesh, geom, 0);
        // Enable linear UV scroll only on translation matrix ids from the .tweak
        // (rotation ids stay static — scrolling them paints camo into the tread loop).
        apply_track_uv_matrices(data, parse_track_uv_matrices(read_vehicle_tweak_text(resources, vpath)));
        const auto& part_xform = hierarchy ? hierarchy->xforms : std::vector<glm::mat4>{};
        std::unordered_set<int> split_parts;
        if (hierarchy) {
          for (const auto& w : hierarchy->wheels) split_parts.insert(w.geom_part);
          for (const auto& g : hierarchy->gear_parts) split_parts.insert(g.geom_part);
        }
        if (!part_xform.empty() && data.vertex_part.size() == data.vertices.size() &&
            !split_parts.empty()) {
          bf2::TexturedMeshData body;
          std::unordered_map<int, bf2::TexturedMeshData> part_meshes;
          std::unordered_map<int, std::unordered_map<std::uint32_t, std::uint32_t>> vmaps;
          auto map_vert = [&](int bucket, std::uint32_t old_i, bf2::TexturedMeshData& target) {
            auto& vm = vmaps[bucket];
            if (auto it = vm.find(old_i); it != vm.end()) return it->second;
            const auto src = data.vertices[old_i];
            bf2::ExtractedVertex v = src;
            const std::size_t p = data.vertex_part[old_i];
            if (bucket < 0) {
              if (p < part_xform.size()) {
                const glm::mat4& m = part_xform[p];
                const glm::vec4 wp =
                    m * glm::vec4(v.position.x, v.position.y, v.position.z, 1.f);
                v.position = {wp.x, wp.y, wp.z};
                const glm::mat3 rot(m);
                const glm::vec3 wn = rot * glm::vec3(v.normal.x, v.normal.y, v.normal.z);
                v.normal = {wn.x, wn.y, wn.z};
              }
            }
            const std::uint32_t ni = static_cast<std::uint32_t>(target.vertices.size());
            target.vertices.push_back(v);
            vm[old_i] = ni;
            return ni;
          };
          // Split by source material so wheel/hull keep correct textures. Only
          // move a triangle onto a wheel/gear mesh when ALL three verts share that
          // part id — mixed-part tris (common on tread/hull seams) must stay on
          // the body. Classifying by the first index alone pulled distant hull
          // verts into the wheel pivot and drew tread strips into the ground.
          auto append_tri = [&](bf2::TexturedMeshData& target, int bucket, std::uint32_t ia,
                                std::uint32_t ib, std::uint32_t ic,
                                const bf2::TexturedSubmesh& src_sm) {
            const std::uint32_t before = static_cast<std::uint32_t>(target.indices.size());
            target.indices.push_back(map_vert(bucket, ia, target));
            target.indices.push_back(map_vert(bucket, ib, target));
            target.indices.push_back(map_vert(bucket, ic, target));
            if (target.submeshes.empty() || target.submeshes.back().base_map != src_sm.base_map ||
                target.submeshes.back().detail_map != src_sm.detail_map ||
                target.submeshes.back().normal_map != src_sm.normal_map ||
                target.submeshes.back().dirt_map != src_sm.dirt_map ||
                target.submeshes.back().crack_map != src_sm.crack_map ||
                target.submeshes.back().animated_uv != src_sm.animated_uv ||
                target.submeshes.back().uv_matrix_id != src_sm.uv_matrix_id ||
                target.submeshes.back().omit_from_hull != src_sm.omit_from_hull ||
                target.submeshes.back().animated_uv_axis_v != src_sm.animated_uv_axis_v) {
              bf2::TexturedSubmesh out = src_sm;
              out.index_offset = before;
              out.index_count = 3;
              target.submeshes.push_back(std::move(out));
            } else {
              target.submeshes.back().index_count += 3;
            }
          };
          const auto emit_range = [&](const bf2::TexturedSubmesh& src_sm) {
            for (std::uint32_t k = 0; k + 2 < src_sm.index_count; k += 3) {
              const std::uint32_t ia = data.indices[src_sm.index_offset + k];
              const std::uint32_t ib = data.indices[src_sm.index_offset + k + 1];
              const std::uint32_t ic = data.indices[src_sm.index_offset + k + 2];
              if (ia >= data.vertex_part.size() || ib >= data.vertex_part.size() ||
                  ic >= data.vertex_part.size()) {
                continue;
              }
              const int pa = static_cast<int>(data.vertex_part[ia]);
              const int pb = static_cast<int>(data.vertex_part[ib]);
              const int pc = static_cast<int>(data.vertex_part[ic]);
              if (pa == pb && pb == pc && split_parts.count(pa)) {
                // Wheel/gear parts keep hub discs even when the material is a
                // camo-atlas UV pad (omit_from_hull).
                bf2::TexturedSubmesh wheel_sm = src_sm;
                wheel_sm.animated_uv = false;  // hubs spin in geometry, not UVs
                wheel_sm.omit_from_hull = false;
                append_tri(part_meshes[pa], pa, ia, ib, ic, wheel_sm);
              } else if (!src_sm.omit_from_hull) {
                append_tri(body, -1, ia, ib, ic, src_sm);
              }
            }
          };
          if (!data.submeshes.empty()) {
            for (const auto& src_sm : data.submeshes) emit_range(src_sm);
          } else {
            bf2::TexturedSubmesh fake;
            fake.index_offset = 0;
            fake.index_count = static_cast<std::uint32_t>(data.indices.size());
            emit_range(fake);
          }
          strip_assembled_track_camo(body);
          bool ok = upload_mesh_data(body, cache_key, vpath);
          for (const auto& w : hierarchy->wheels) {
            auto wit = part_meshes.find(w.geom_part);
            if (wit == part_meshes.end() || wit->second.vertices.empty()) continue;
            const std::string wkey = vpath + "#wheel_" + std::to_string(w.geom_part);
            const float wheel_low = transformed_mesh_low_y(wit->second, w.rest);
            upload_mesh_data(wit->second, wkey, vpath, wheel_low);
          }
          for (const auto& g : hierarchy->gear_parts) {
            auto git = part_meshes.find(g.geom_part);
            if (git == part_meshes.end() || git->second.vertices.empty()) continue;
            const std::string gkey = vpath + "#gear_" + std::to_string(g.geom_part);
            const float gear_low = transformed_mesh_low_y(git->second, g.rest);
            upload_mesh_data(git->second, gkey, vpath, gear_low);
          }
          return ok;
        }
        if (!part_xform.empty() && data.vertex_part.size() == data.vertices.size()) {
          for (std::size_t vi = 0; vi < data.vertices.size(); ++vi) {
            const std::size_t p = data.vertex_part[vi];
            if (p >= part_xform.size()) continue;
            const glm::mat4& m = part_xform[p];
            auto& v = data.vertices[vi];
            const glm::vec4 wp = m * glm::vec4(v.position.x, v.position.y, v.position.z, 1.f);
            v.position = {wp.x, wp.y, wp.z};
            const glm::mat3 rot(m);
            const glm::vec3 wn = rot * glm::vec3(v.normal.x, v.normal.y, v.normal.z);
            v.normal = {wn.x, wn.y, wn.z};
          }
        }
        if (!data.vertices.empty()) {
          strip_assembled_track_camo(data);
          return upload_mesh_data(data, cache_key, vpath);
        }
      } catch (const std::exception&) {
      }
      const bool ok = gpu.vao != 0;
      vehicle_cache[cache_key] = std::move(gpu);
      return ok;
    };

    // Parse a vehicle's .tweak/.con to find child part meshes and their offsets.
    // We read every `addTemplate NAME` followed by optional setPosition/setRotation
    // and keep only those whose NAME resolves to a *separate* bundledmesh (rotors,
    // etc.). Names that don't (geometryParts baked into the body, sounds, cameras,
    // effects) are skipped -- their geometry is already in the body or is non-visual.
    auto parse_vehicle_parts = [&](const std::string& body_vpath) -> std::vector<VehiclePart> {
      std::vector<VehiclePart> parts;
      // loaded_vehicles/<cat>/<name>/meshes/<name>.bundledmesh -> folder loaded_vehicles/<cat>/<name>
      const auto meshes_pos = body_vpath.rfind("/meshes/");
      if (meshes_pos == std::string::npos) return parts;
      const std::string folder = body_vpath.substr(0, meshes_pos);
      const std::string name = bf2::detail::basename_no_ext(body_vpath);
      std::string text;
      for (const char* ext : {".tweak", ".con"}) {
        if (const auto b = resources.read_bytes(folder + "/" + name + ext)) {
          text.append(reinterpret_cast<const char*>(b->data()), b->size());
          text.push_back('\n');
        }
      }
      if (text.empty()) return parts;

      auto parse_triple = [](const std::string& s, glm::vec3& out) {
        // "x/y/z" -> vec3
        float v[3] = {0, 0, 0};
        int n = 0;
        std::size_t start = 0;
        for (int k = 0; k < 3; ++k) {
          const auto slash = s.find('/', start);
          const std::string tok = s.substr(start, slash == std::string::npos ? std::string::npos
                                                                              : slash - start);
          try {
            v[k] = std::stof(tok);
            ++n;
          } catch (...) {
          }
          if (slash == std::string::npos) break;
          start = slash + 1;
        }
        out = glm::vec3(v[0], v[1], v[2]);
        return n == 3;
      };

      std::istringstream ss(text);
      std::string line;
      int cur = -1;  // index of the last addTemplate child (pending pos/rot)
      glm::vec3 cur_pos(0.f), cur_rot(0.f);
      auto commit = [&]() {
        if (cur < 0) return;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), cur_pos);
        m = glm::rotate(m, glm::radians(cur_rot.x), glm::vec3(0, 1, 0));  // yaw
        m = glm::rotate(m, glm::radians(cur_rot.y), glm::vec3(1, 0, 0));  // pitch
        m = glm::rotate(m, glm::radians(cur_rot.z), glm::vec3(0, 0, 1));  // roll
        parts[cur].local = m;
        cur = -1;
      };
      while (std::getline(ss, line)) {
        // Tokenize: "ObjectTemplate.<cmd> <arg...>"
        std::istringstream ls(line);
        std::string cmd, arg;
        ls >> cmd >> arg;
        auto lc = cmd;
        for (auto& c : lc) c = static_cast<char>(std::tolower((unsigned char)c));
        if (lc == "objecttemplate.addtemplate") {
          commit();
          std::string child = arg;
          std::string cl = child;
          for (auto& c : cl) c = static_cast<char>(std::tolower((unsigned char)c));
          const auto mit = bundled_by_name.find(cl);
          // Don't re-attach the body itself; only separate child meshes.
          if (mit != bundled_by_name.end() && mit->second != body_vpath) {
            VehiclePart p;
            p.mesh_key = mit->second;
            parts.push_back(p);
            cur = static_cast<int>(parts.size()) - 1;
            cur_pos = glm::vec3(0.f);
            cur_rot = glm::vec3(0.f);
          } else {
            cur = -1;  // not a mesh child; ignore its pos/rot
          }
        } else if (lc == "objecttemplate.setposition" && cur >= 0) {
          parse_triple(arg, cur_pos);
        } else if (lc == "objecttemplate.setrotation" && cur >= 0) {
          parse_triple(arg, cur_rot);
        } else if (lc == "objecttemplate.create") {
          commit();  // a new object definition begins; finalize any pending child
        }
      }
      commit();
      return parts;
    };

    // Build per-geometry-part transforms from a vehicle's .con object hierarchy.
    // BF2 loaded_vehicles bake turret/barrel/wheels into one bundledmesh as separate
    // rigid parts (each `geometryPart N`), positioned by an ObjectTemplate tree:
    // the root PhysicalObject owns part 0 (hull) at identity, and every
    // addTemplate/setPosition/setRotation nests a child part relative to it.
    auto build_part_transforms = [&](const std::string& body_vpath) -> PartHierarchy {
      PartHierarchy hierarchy;
      const auto meshes_pos = body_vpath.rfind("/meshes/");
      if (meshes_pos == std::string::npos) return hierarchy;
      const std::string folder = body_vpath.substr(0, meshes_pos);
      const std::string name = bf2::detail::basename_no_ext(body_vpath);
      std::string text;
      for (const char* ext : {".con", ".tweak"}) {
        if (const auto b = resources.read_bytes(folder + "/" + name + ext)) {
          text.append(reinterpret_cast<const char*>(b->data()), b->size());
          text.push_back('\n');
        }
      }
      if (text.empty()) return hierarchy;

      auto lc = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
        return s;
      };
      const auto is_wheel_name = [&](const std::string& n) {
        const std::string s = lc(n);
        return s.find("wheel") != std::string::npos || s.find("tire") != std::string::npos ||
               s.find("tyre") != std::string::npos || s.find("sprocket") != std::string::npos ||
               s.find("drivewheel") != std::string::npos || s.find("driwewheel") != std::string::npos;
      };
      const auto is_sprocket_wheel = [&](const std::string& n, const std::string& type) {
        const std::string s = lc(n);
        const std::string t = lc(type);
        if (t == "rotationalbundle") return true;
        return s.find("sprocket") != std::string::npos || s.find("driwe") != std::string::npos ||
               s.find("drivewheel") != std::string::npos;
      };
      const auto is_steer_name = [&](const std::string& n) {
        const std::string s = lc(n);
        return s.find("front") != std::string::npos || s.find("_fl") != std::string::npos ||
               s.find("_fr") != std::string::npos || s.find("steer") != std::string::npos;
      };
      auto parse_triple = [](const std::string& s) {
        float v[3] = {0, 0, 0};
        std::size_t start = 0;
        for (int k = 0; k < 3; ++k) {
          const auto slash = s.find('/', start);
          try {
            v[k] = std::stof(s.substr(start, slash == std::string::npos ? std::string::npos
                                                                        : slash - start));
          } catch (...) {
          }
          if (slash == std::string::npos) break;
          start = slash + 1;
        }
        return glm::vec3(v[0], v[1], v[2]);
      };
      struct Child {
        std::string name;
        glm::vec3 pos{0.f};
        glm::vec3 rot{0.f};
      };
      struct ObjDef {
        int geom_part = -1;
        std::string type;
        std::vector<Child> children;
      };
      std::unordered_map<std::string, ObjDef> objs;
      std::string cur;
      std::string first;
      int cur_child = -1;  // index into objs[cur].children awaiting pos/rot
      std::istringstream ss(text);
      std::string line;
      while (std::getline(ss, line)) {
        std::istringstream ls(line);
        std::string cmd, arg;
        ls >> cmd >> arg;
        const std::string k = lc(cmd);
        if (k == "objecttemplate.create") {
          std::string oname;
          ls >> oname;  // arg holds the type, next token is the name
          cur = lc(oname);
          if (first.empty()) first = cur;
          objs[cur].type = lc(arg);
          cur_child = -1;
        } else if (k == "objecttemplate.geometrypart" && !cur.empty()) {
          try {
            objs[cur].geom_part = std::stoi(arg);
          } catch (...) {
          }
        } else if (k == "objecttemplate.addtemplate" && !cur.empty()) {
          Child c;
          c.name = lc(arg);
          objs[cur].children.push_back(c);
          cur_child = static_cast<int>(objs[cur].children.size()) - 1;
        } else if (k == "objecttemplate.setposition" && cur_child >= 0) {
          objs[cur].children[cur_child].pos = parse_triple(arg);
        } else if (k == "objecttemplate.setrotation" && cur_child >= 0) {
          objs[cur].children[cur_child].rot = parse_triple(arg);
        }
      }
      // Root: the object matching the mesh base name, else the first created.
      std::string root = objs.count(lc(name)) ? lc(name) : first;
      if (root.empty()) return hierarchy;

      const auto tuck_for_wheel = [&](const std::string& n) {
        const std::string s = lc(n);
        if (s.find("fwheel") != std::string::npos || s.find("nose") != std::string::npos)
          return std::pair<float, int>{92.f, 1};
        return std::pair<float, int>{60.f, 1};
      };
      const auto tuck_for_gear = [&](const std::string& n) {
        const std::string s = lc(n);
        if (s.find("hatch") != std::string::npos) return std::pair<float, int>{90.f, 2};
        if (s.find("fgear") != std::string::npos) return std::pair<float, int>{92.f, 1};
        return std::pair<float, int>{60.f, 1};
      };
      std::unordered_map<std::string, bool> visited;
      // Iterative DFS carrying the accumulated transform for each node.
      std::vector<std::pair<std::string, glm::mat4>> stack;
      stack.emplace_back(root, glm::mat4(1.0f));
      while (!stack.empty()) {
        auto [oname, xform] = stack.back();
        stack.pop_back();
        if (visited[oname]) continue;
        visited[oname] = true;
        auto it = objs.find(oname);
        if (it == objs.end()) continue;
        int gp = it->second.geom_part;
        if (oname == root && gp < 0) gp = 0;  // hull is the implicit part 0
        if (gp >= 0) {
          if (static_cast<int>(hierarchy.xforms.size()) <= gp)
            hierarchy.xforms.resize(gp + 1, glm::mat4(1.0f));
          hierarchy.xforms[gp] = xform;
          const std::string& otype = it->second.type;
          if (is_wheel_name(oname)) {
            dalian::VehicleWheelSlot ws;
            ws.geom_part = gp;
            ws.rest = xform;
            ws.steers = is_steer_name(oname);
            ws.is_sprocket = is_sprocket_wheel(oname, otype);
            const auto [ang, ax] = tuck_for_wheel(oname);
            ws.gear_tuck_angle = ang;
            ws.gear_tuck_axis = ax;
            hierarchy.wheels.push_back(std::move(ws));
          } else if (otype == "landinggear" ||
                     (oname.find("gear") != std::string::npos && oname.find("wheel") == std::string::npos)) {
            dalian::VehicleGearSlot gs;
            gs.geom_part = gp;
            gs.rest = xform;
            const auto [ang, ax] = tuck_for_gear(oname);
            gs.gear_tuck_angle = ang;
            gs.gear_tuck_axis = ax;
            hierarchy.gear_parts.push_back(std::move(gs));
          }
        }
        for (const auto& c : it->second.children) {
          glm::mat4 m = glm::translate(xform, c.pos);
          m = glm::rotate(m, glm::radians(c.rot.x), glm::vec3(0, 1, 0));  // yaw
          m = glm::rotate(m, glm::radians(c.rot.y), glm::vec3(1, 0, 0));  // pitch
          m = glm::rotate(m, glm::radians(c.rot.z), glm::vec3(0, 0, 1));  // roll
          stack.emplace_back(c.name, m);
        }
      }
      return hierarchy;
    };

    const auto spawns = parse_vehicle_spawns(vehicle_script);
    int placed = 0;
    int part_count = 0;
    std::unordered_map<std::string, std::vector<VehiclePart>> parts_cache;
    std::unordered_map<std::string, PartHierarchy> xform_cache;
    for (const auto& vs : spawns) {
      const auto mit = bundled_by_name.find(vs.vehicle);
      if (mit == bundled_by_name.end()) {
        if (std::getenv("BF2_VEHLIST"))
          std::cerr << "  UNRESOLVED spawn vehicle='" << vs.vehicle << "'\n";
        continue;
      }
      const std::string& vpath = mit->second;
      auto xcit = xform_cache.find(vpath);
      if (xcit == xform_cache.end())
        xcit = xform_cache.emplace(vpath, build_part_transforms(vpath)).first;
      if (!load_vehicle_mesh(vpath, &xcit->second)) continue;

      // Assemble (and cache) the child-part list for this vehicle type.
      auto pcit = parts_cache.find(vpath);
      if (pcit == parts_cache.end()) {
        auto parts = parse_vehicle_parts(vpath);
        // Keep only parts whose mesh actually loads.
        std::vector<VehiclePart> good;
        for (auto& p : parts) {
          if (p.mesh_key.find("rotor") != std::string::npos) {
            // Rotor meshes carry two geometries: geom0 = static blades (shown when
            // parked/spooling), geom1 = blur disc (shown at speed). Load both.
            const bool blades_ok = load_vehicle_mesh(p.mesh_key, nullptr, 0, p.mesh_key);
            load_vehicle_mesh(p.mesh_key, nullptr, 1, p.mesh_key + "#blur");
            if (blades_ok) good.push_back(std::move(p));
          } else if (load_vehicle_mesh(p.mesh_key)) {
            good.push_back(std::move(p));
          }
        }
        if (std::getenv("BF2_PARTDBG")) {
          for (const auto& p : good)
            std::cerr << "  PART " << bf2::detail::basename_no_ext(vpath) << " <- "
                      << bf2::detail::basename_no_ext(p.mesh_key) << "\n";
        }
        pcit = parts_cache.emplace(vpath, std::move(good)).first;
      }

      Vehicle v;
      v.mesh_key = vpath;
      v.wheels = xcit->second.wheels;
      for (auto& w : v.wheels) {
        w.mesh_key = vpath + "#wheel_" + std::to_string(w.geom_part);
      }
      v.gear_parts = xcit->second.gear_parts;
      for (auto& g : v.gear_parts) {
        g.mesh_key = vpath + "#gear_" + std::to_string(g.geom_part);
      }
      v.wheel_spin.assign(v.wheels.size(), 0.f);
      v.is_air = dalian::vehicle_is_aircraft(vpath, vs.vehicle);
      v.is_heli = dalian::vehicle_is_helicopter(vpath, vs.vehicle);
      v.is_boat = dalian::vehicle_is_boat(vpath, vs.vehicle);
      if (const auto hit = vehicle_half_xz.find(vpath); hit != vehicle_half_xz.end()) {
        // Trim slightly so the player can brush right up against the hull, and
        // keep a sane minimum so tiny/odd meshes still block.
        v.col_half = glm::max(hit->second * 0.9f, glm::vec3(1.2f, 0.8f, 1.6f));
      }
      // Ground clearance: how far the origin should ride above the surface it
      // rests on so the wheels/tracks/skids touch down. The authored spawn Y sits
      // the vehicle correctly, but it's given in WORLD space — so on a raised deck
      // (aircraft-carrier flight deck, bridge, rooftop helipad) the height above
      // bare terrain also includes the whole deck, which would bury the gear in
      // the deck. Probe downward from the spawn for the real resting surface first
      // and measure the clearance relative to THAT, so wheels sit on the deck the
      // same way they sit on a runway.
      //
      // Ground spawns under open hangars/canopies must NOT snap to the roof: only
      // look for elevated collision when the authored Y is well above terrain.
      const float terrY = world.terrain_height(vs.pos.x, vs.pos.z);
      const float scriptY = vs.pos.y;
      float surfY = terrY;
      if (scriptY > terrY + 1.5f) {
        // Helipad, carrier deck, bridge — find walkable surface near authored height.
        const float support =
            world.support_height(vs.pos.x, vs.pos.z, scriptY + 1.f, 3.f);
        if (support >= scriptY - 2.f && support <= scriptY + 3.f)
          surfY = std::max(terrY, support);
        const auto dn =
            world.raycast({vs.pos.x, scriptY + 1.5f, vs.pos.z}, {0.f, -1.f, 0.f}, 25.f);
        if (dn.hit && std::fabs(dn.normal.y) > 0.35f && dn.point.y >= terrY + 0.5f &&
            dn.point.y >= scriptY - 2.f && dn.point.y <= scriptY + 4.f)
          surfY = std::max(surfY, dn.point.y);
      } else {
        // Runway / parking — stay on terrain; short downward probe only (skips roofs).
        surfY = terrY;
        const auto dn =
            world.raycast({vs.pos.x, scriptY + 0.5f, vs.pos.z}, {0.f, -1.f, 0.f}, 5.f);
        if (dn.hit && std::fabs(dn.normal.y) > 0.35f && dn.point.y >= terrY - 0.75f &&
            dn.point.y <= scriptY + 0.75f)
          surfY = dn.point.y;
      }
      if (v.is_boat && terrY < atmo.water_level + 0.5f && surfY < atmo.water_level) {
        surfY = atmo.water_level;
      }
      const float script_off = vs.pos.y - surfY;
      const auto myit = vehicle_min_y.find(vpath);
      const float body_low = myit != vehicle_min_y.end() ? myit->second : 0.f;
      // Lowest rendered point including child parts (wheels/gear are often separate
      // geometry parts, not in the body mesh).
      float low = body_low;
      for (const auto& w : v.wheels) {
        const auto pit = vehicle_min_y.find(w.mesh_key);
        if (pit != vehicle_min_y.end()) low = std::min(low, pit->second);
      }
      for (const auto& g : v.gear_parts) {
        const auto pit = vehicle_min_y.find(g.mesh_key);
        if (pit != vehicle_min_y.end()) low = std::min(low, pit->second);
      }
      for (const auto& p : pcit->second) {
        const auto pit = vehicle_min_y.find(p.mesh_key);
        if (pit != vehicle_min_y.end()) low = std::min(low, p.local[3].y + pit->second);
      }
      const float mesh_off = -low;
      // Always keep at least the mesh gear/skid offset above the surface; honour a
      // larger authored offset when BF2 placed the vehicle higher (runway spawns).
      v.clearance = std::max(mesh_off, script_off > 0.05f ? script_off : mesh_off);
      v.land_clearance = std::max(0.f, mesh_off);
      if (std::getenv("BF2_VEHLIST"))
        std::cerr << "  CLR " << bf2::detail::basename_no_ext(vpath) << " scriptY=" << vs.pos.y
                  << " terrY=" << terrY << " surfY=" << surfY << " script_off=" << script_off
                  << " body_low=" << body_low << " part_low=" << low
                  << " mesh_off=" << mesh_off << " -> clearance=" << v.clearance << '\n';
      // Rest the vehicle on the surface (terrain or deck) so wheels/tracks/skids
      // touch down. Parked and driven use the same rule so a vehicle never jumps
      // height when you get in.
      glm::vec3 place = vs.pos;
      place.y = surfY + v.clearance;
      v.origin = place;
      v.pos = place;
      v.spawn_pos = place;
      v.spawn_heading = vs.rot.x;
      v.spawn_pitch = vs.rot.y;
      v.spawn_roll = vs.rot.z;
      v.heading = vs.rot.x;
      glm::mat4 m = glm::translate(glm::mat4(1.0f), place);
      m = glm::rotate(m, glm::radians(vs.rot.x), glm::vec3(0, 1, 0));
      m = glm::rotate(m, glm::radians(vs.rot.y), glm::vec3(1, 0, 0));
      m = glm::rotate(m, glm::radians(vs.rot.z), glm::vec3(0, 0, 1));
      v.model = m;
      v.parts = pcit->second;
      if (v.is_air) {
        if (!v.is_heli) {
          v.throttle = 0.f;
          v.jet_rpm = 0.f;
          v.jet_airborne = false;
          v.jet_gear_down = true;
          v.jet_gear_anim = 0.f;
          v.jet_sprint = 1.f;
          v.vel = glm::vec3(0.f);
          v.pitch = 0.f;
          v.roll = 0.f;
          v.wheels_on_ground = true;
        }
      }
      {
        const auto meshes_pos = vpath.rfind("/meshes/");
        bool tracked = vehicle_path_is_tracked(vpath);
        std::string tweak_text;
        if (meshes_pos != std::string::npos) {
          const std::string folder = vpath.substr(0, meshes_pos);
          const std::string name = bf2::detail::basename_no_ext(vpath);
          for (const char* ext : {".tweak", ".con"}) {
            if (const auto tb = resources.read_bytes(folder + "/" + name + ext)) {
              tweak_text.append(reinterpret_cast<const char*>(tb->data()), tb->size());
              tweak_text.push_back('\n');
            }
          }
          if (!tweak_text.empty()) tracked = vehicle_tweak_is_tracked(tweak_text);
        }
        v.is_tracked = tracked;
        for (auto& w : v.wheels) {
          // Spin every wheel disc (road wheels + sprockets). Tracked vehicles used
          // to UV-scroll wheel meshes instead, but those parts share the hull atlas
          // so scrolling made the hubs crawl/change colour. BF2 animates the tread
          // belt via per-material UV matrices on the hull; hubs rotate in place.
          w.spin_geometry = true;
        }
        if (!tweak_text.empty()) {
          if (v.is_air) {
            const dalian::VehicleAirProfile air = dalian::parse_vehicle_air_profile(tweak_text);
            dalian::apply_vehicle_air_profile(v, air);
            v.jet_sprint = v.sprint_limit;
          }
          v.weapons = dalian::parse_vehicle_weapons(tweak_text);
          dalian::resolve_vehicle_weapon_projectiles(resources, tweak_text, v.weapons);
        }
      }
      // A rotor part means this is a helicopter (not a jet): it flies the cyclic
      // model well and carries a co-pilot/gunner station.
      for (const auto& p : v.parts) {
        if (p.mesh_key.find("rotor") != std::string::npos) {
          v.has_gunner_seat = true;
          v.is_heli = true;
          break;
        }
      }
      if (v.is_heli) v.is_air = true;
      // Crew seat layout by vehicle class. Seat 0 always drives/pilots.
      const bool is_tank = vpath.find("loaded_vehicles/land/") != std::string::npos &&
                           (vpath.find("tnk") != std::string::npos ||
                            vpath.find("apc") != std::string::npos ||
                            vpath.find("aav") != std::string::npos);
      if (v.is_air && v.has_gunner_seat) {
        v.seats = {{"PILOT", -1}, {"GUNNER", -1}};  // attack/transport helicopter
      } else if (v.is_air) {
        v.seats = {{"PILOT", -1}};  // fixed-wing jet
      } else if (is_tank) {
        v.seats = {{"DRIVER", -1}, {"GUNNER", -1}};
      } else {
        v.seats = {{"DRIVER", -1}, {"GUNNER", -1}, {"PASSENGER", -1}};  // jeep/buggy
      }
      part_count += static_cast<int>(v.parts.size());
      loaded_vehicles.push_back(std::move(v));
      ++placed;
    }
    std::cout << "Vehicles: " << placed << " placed, " << vehicle_cache.size()
              << " unique meshes, " << part_count << " attached parts\n";
    load_step(0.62f, "LOADING VEHICLES", "Placing vehicles and weapons...");
    if (std::getenv("BF2_VEHLIST")) {
      for (const auto& v : loaded_vehicles)
        std::cerr << "  veh " << v.mesh_key << " parts=" << v.parts.size()
                  << " clearance=" << v.clearance << " terrainY="
                  << world.terrain_height(v.origin.x, v.origin.z) << " originY=" << v.origin.y
                  << " air=" << v.is_air << " col_half=" << v.col_half.x << "," << v.col_half.y
                  << "," << v.col_half.z << '\n';
    }
  }

  // Swappable first-person weapons: each entry is a real BF2 rifle whose
  // geometry[0] is the 1p viewmodel. Meshes are loaded lazily and cached.
  struct WeaponDef {
    const char* name;
    const char* vpath;
  };
  const std::vector<WeaponDef> weapon_defs = {
      {"M4", "weapons/handheld/usrif_m4/meshes/usrif_m4.bundledmesh"},
      {"M16A2", "weapons/handheld/usrif_m16a2/meshes/usrif_m16a2.bundledmesh"},
      {"AK-47", "weapons/handheld/rurif_ak47/meshes/rurif_ak47.bundledmesh"},
      {"AK-101", "weapons/handheld/rurif_ak101/meshes/rurif_ak101.bundledmesh"},
      {"G36E", "weapons/handheld/sasrif_g36e/meshes/sasrif_g36e.bundledmesh"},
      {"G3A3", "weapons/handheld/usrif_g3a3/meshes/usrif_g3a3.bundledmesh"},
  };
  static const char* kWeaponTweakPaths[] = {
      "weapons/handheld/usrif_m4/usrif_m4.tweak",
      "weapons/handheld/usrif_m16a2/usrif_m16a2.tweak",
      "weapons/handheld/rurif_ak47/rurif_ak47.tweak",
      "weapons/handheld/rurif_ak101/rurif_ak101.tweak",
      "weapons/handheld/sasrif_g36e/sasrif_g36e.tweak",
      "weapons/handheld/usrif_g3a3/usrif_g3a3.tweak",
  };
  std::vector<dalian::WeaponProfile> weapon_profiles(weapon_defs.size());
  for (std::size_t i = 0; i < weapon_defs.size(); ++i) {
    if (i < std::size(kWeaponTweakPaths)) {
      weapon_profiles[i] = dalian::load_weapon_profile(resources, kWeaponTweakPaths[i]);
    }
  }
  std::vector<bf2::GpuTexturedMesh> weapon_meshes(weapon_defs.size());
  std::vector<bool> weapon_loaded(weapon_defs.size(), false);

  auto load_weapon = [&](std::size_t idx) -> bool {
    if (idx >= weapon_defs.size()) return false;
    if (weapon_loaded[idx]) return weapon_meshes[idx].vao != 0;
    weapon_loaded[idx] = true;
    try {
      const auto wm = resources.load_mesh(weapon_defs[idx].vpath);
      const auto wdata = bf2::MeshLoader::extract_textured(wm, 0, 0);  // 1p geom, LOD0
      if (!wdata.vertices.empty()) {
        auto gpu = renderer.upload_textured(wdata);
        for (std::size_t i = 0; i < gpu.submeshes.size() && i < wdata.submeshes.size(); ++i) {
          gpu.submeshes[i].base_tex = textures.get(wdata.submeshes[i].base_map);
          gpu.submeshes[i].detail_tex = textures.get(wdata.submeshes[i].detail_map);
          gpu.submeshes[i].normal_tex = textures.get(wdata.submeshes[i].normal_map);
          gpu.submeshes[i].dirt_tex = textures.get(wdata.submeshes[i].dirt_map);
          gpu.submeshes[i].crack_tex = textures.get(wdata.submeshes[i].crack_map);
          gpu.submeshes[i].specular_tex = textures.get(wdata.submeshes[i].specular_map);
        }
        weapon_meshes[idx] = std::move(gpu);
      }
    } catch (const std::exception&) {
    }
    return weapon_meshes[idx].vao != 0;
  };

  std::size_t weapon_index = 0;
  for (std::size_t i = 0; i < weapon_defs.size(); ++i) {
    if (load_weapon(i)) {
      weapon_index = i;
      break;
    }
  }
  const bool have_weapon_model = weapon_meshes[weapon_index].vao != 0;
  std::cout << "Weapon: " << (have_weapon_model ? weapon_defs[weapon_index].name : "procedural")
            << " (" << weapon_defs.size() << " available, wheel/Q to swap)\n";

  // ---- BF2 in-game audio (weapons, voice, level ambient from retail archives) ---
  dalian::GameAudio game_audio;
  dalian::VoiceBank voice_bank;
  std::vector<dalian::WeaponSoundSet> weapon_sound_sets(weapon_defs.size());
  std::unordered_map<std::string, dalian::VehicleSoundSet> vehicle_sounds;
  int veh_engine_ch = -1;
  int veh_tire_ch = -1;
  int veh_audio_idx = -1;
  bool have_game_audio = false;
  if (!headless_shot) {
    have_game_audio = game_audio.init(settings.master_volume, settings.sfx_volume);
    if (have_game_audio) {
      voice_bank.load_from_archives(resources);
      static const char* kWeaponTweaks[] = {
          "weapons/handheld/usrif_m4/usrif_m4.tweak",
          "weapons/handheld/usrif_m16a2/usrif_m16a2.tweak",
          "weapons/handheld/rurif_ak47/rurif_ak47.tweak",
          "weapons/handheld/rurif_ak101/rurif_ak101.tweak",
          "weapons/handheld/sasrif_g36e/sasrif_g36e.tweak",
          "weapons/handheld/usrif_g3a3/usrif_g3a3.tweak",
      };
      for (std::size_t i = 0; i < weapon_defs.size() && i < std::size(kWeaponTweaks); ++i) {
        if (game_audio.load_weapon_sounds(resources, kWeaponTweaks[i], weapon_sound_sets[i])) {
          std::cout << "GameAudio: " << weapon_defs[i].name << " weapon sounds loaded\n";
        }
      }
      std::unordered_set<std::string> seen_veh_snd;
      for (const auto& vv : loaded_vehicles) {
        if (!seen_veh_snd.insert(vv.mesh_key).second) continue;
        const auto mp = vv.mesh_key.rfind("/meshes/");
        if (mp == std::string::npos) continue;
        const std::string folder = vv.mesh_key.substr(0, mp);
        const std::string name = bf2::detail::basename_no_ext(vv.mesh_key);
        dalian::VehicleSoundSet vs{};
        if (game_audio.load_vehicle_sounds(resources, folder + "/" + name + ".tweak", vs)) {
          vehicle_sounds[vv.mesh_key] = vs;
          std::cout << "GameAudio: vehicle sounds for " << name << '\n';
        }
      }
      if (!weapon_sound_sets[weapon_index].fire_1p.empty())
        game_audio.set_weapon_sounds(&weapon_sound_sets[weapon_index]);
    }
  }
  auto bind_weapon_audio = [&]() {
    if (have_game_audio && weapon_index < weapon_sound_sets.size() &&
        !weapon_sound_sets[weapon_index].fire_1p.empty()) {
      game_audio.set_weapon_sounds(&weapon_sound_sets[weapon_index]);
    }
  };
  auto bind_weapon_sim = [&](bool refill_ammo) {
    if (weapon_index < weapon_profiles.size() && weapon_profiles[weapon_index].valid) {
      auto wp = weapon_profiles[weapon_index];
      wp.name = weapon_defs[weapon_index].name;
      game_sim.set_weapon_profile(wp, refill_ammo);
    }
  };
  float voice_cooldown = 0.f;
  float snapshot_log_timer = 0.f;

  bf2::GpuColorMesh gun_mesh{};
  if (!have_weapon_model) {
    std::vector<float> gv;
    std::vector<std::uint32_t> gi;
    build_gun_mesh(gv, gi);
    gun_mesh = renderer.upload_color(gv, gi);
  }

  // ---- Missile mesh: prefer custom content OBJ (e.g. Storm Shadow), else BF2 ----
  bf2::GpuTexturedMesh missile_mesh{};
  bf2::GpuTexturedMesh tracer_mesh{};
  std::uint32_t tracer_tex = 0;
  bool missile_mesh_custom = false;
  float missile_draw_scale_sam = 14.f;  // BF2 weapon meshes are tiny
  {
    namespace fs = std::filesystem;
    std::vector<fs::path> custom_candidates;
    if (const char* env = std::getenv("BF2_MISSILE_OBJ")) custom_candidates.emplace_back(env);
    // Dev tree + next-to-exe content pack
    custom_candidates.emplace_back("apps/dalian/content/missiles/storm_shadow/storm_shadow.obj");
    custom_candidates.emplace_back("content/missiles/storm_shadow/storm_shadow.obj");
    custom_candidates.emplace_back("../content/missiles/storm_shadow/storm_shadow.obj");
    custom_candidates.emplace_back("../../apps/dalian/content/missiles/storm_shadow/storm_shadow.obj");
    // Also try beside the executable (Release/Debug run dirs).
    {
      const fs::path exe = fs::current_path();
      custom_candidates.emplace_back(exe / "content/missiles/storm_shadow/storm_shadow.obj");
      custom_candidates.emplace_back(exe / "../content/missiles/storm_shadow/storm_shadow.obj");
      custom_candidates.emplace_back(exe /
                                     "../../../apps/dalian/content/missiles/storm_shadow/storm_shadow.obj");
      custom_candidates.emplace_back(exe /
                                     "../../../../apps/dalian/content/missiles/storm_shadow/storm_shadow.obj");
    }
    std::string mpath;
    for (const auto& cand : custom_candidates) {
      std::error_code ec;
      if (fs::is_regular_file(cand, ec)) {
        try {
          auto data = bf2::load_obj_file(cand.string());
          if (!data.vertices.empty() && !data.indices.empty()) {
            missile_mesh = renderer.upload_textured(data);
            missile_mesh_custom = missile_mesh.vao != 0;
            if (missile_mesh_custom) {
              // OBJ is normalized to length 1; Storm Shadow ≈ 5.1 m.
              missile_draw_scale_sam = 5.2f;
              mpath = cand.string();
              break;
            }
          }
        } catch (const std::exception&) {
        }
      }
    }
    if (!missile_mesh_custom) {
      const std::vector<const char*> pref = {"igla_9k38", "ru_sam_9m311", "9m311", "ru_sam_sa18",
                                             "aim9m_sidewinder"};
      const auto all = resources.archives().list();
      auto is_bundled = [](const std::string& p) {
        return p.size() > 12 && p.compare(p.size() - 12, 12, ".bundledmesh") == 0;
      };
      for (const char* want : pref) {
        for (const auto& p : all) {
          if (is_bundled(p) && bf2::detail::basename_no_ext(p) == want) {
            mpath = p;
            break;
          }
        }
        if (!mpath.empty()) break;
      }
      if (mpath.empty()) {
        for (const auto& p : all) {
          if (is_bundled(p) && (p.find("missile") != std::string::npos ||
                                p.find("_sam_") != std::string::npos ||
                                p.find("rocket") != std::string::npos)) {
            mpath = p;
            break;
          }
        }
      }
      if (!mpath.empty()) {
        try {
          const auto mesh = resources.load_mesh(mpath);
          std::size_t best = 0;
          std::uint32_t best_n = 0;
          for (std::size_t g = 0; g < mesh.geometries.size(); ++g) {
            if (mesh.geometries[g].lods.empty()) continue;
            std::uint32_t n = 0;
            for (const auto& mm : mesh.geometries[g].lods[0].materials) n += mm.index_count;
            if (n > best_n) {
              best_n = n;
              best = g;
            }
          }
          const auto data = bf2::MeshLoader::extract_textured(mesh, best, 0);
          if (!data.vertices.empty()) {
            missile_mesh = renderer.upload_textured(data);
            for (std::size_t i = 0; i < missile_mesh.submeshes.size() && i < data.submeshes.size();
                 ++i) {
              missile_mesh.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
              missile_mesh.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
              missile_mesh.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map);
              missile_mesh.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map);
              missile_mesh.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map);
              missile_mesh.submeshes[i].specular_tex =
                  textures.get(data.submeshes[i].specular_map);
            }
          }
        } catch (const std::exception&) {
        }
      }
    }
    std::cout << "Missile: "
              << (missile_mesh.vao != 0
                      ? (missile_mesh_custom ? std::string("custom OBJ ") + mpath : mpath)
                      : std::string("procedural (no mesh found)"))
              << '\n';
  }

  // ---- MIM-23 Hawk emplacement (custom OBJ) — open ground by Chinese Z-8 ----
  bf2::GpuTexturedMesh hawk_mesh{};
  glm::vec3 hawk_pos{0.f};
  bool hawk_present = false;
  float hawk_heading_init = 0.f;
  float hawk_draw_scale = 2.0f;
  float hawk_ground_lift = 0.f;
  // Source FBX/OBJ sits on Z=0 (Z-up). Convert to engine Y-up before draw.
  glm::mat4 hawk_mesh_fix =
      glm::rotate(glm::mat4(1.f), glm::radians(-90.f), glm::vec3(1.f, 0.f, 0.f));
  {
    namespace fs = std::filesystem;
    std::vector<fs::path> cands;
    if (const char* env = std::getenv("BF2_HAWK_OBJ")) cands.emplace_back(env);
    cands.emplace_back("content/emplacements/mim23_hawk/mim23_launcher.obj");
    cands.emplace_back("apps/dalian/content/emplacements/mim23_hawk/mim23_launcher.obj");
    cands.emplace_back("../content/emplacements/mim23_hawk/mim23_launcher.obj");
    cands.emplace_back("../../apps/dalian/content/emplacements/mim23_hawk/mim23_launcher.obj");
    if (char* base = SDL_GetBasePath()) {
      const fs::path exe_dir(base);
      SDL_free(base);
      cands.insert(cands.begin(),
                   exe_dir / "content/emplacements/mim23_hawk/mim23_launcher.obj");
      cands.insert(cands.begin() + 1,
                   exe_dir / "../content/emplacements/mim23_hawk/mim23_launcher.obj");
    }
    cands.emplace_back(fs::current_path() / "content/emplacements/mim23_hawk/mim23_launcher.obj");
    cands.emplace_back(fs::current_path() /
                       "../../../apps/dalian/content/emplacements/mim23_hawk/mim23_launcher.obj");

    auto solid_tex = [&](float r, float g, float b) -> std::uint32_t {
      std::uint8_t px[16 * 4];
      for (int i = 0; i < 16; ++i) {
        px[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(r, 0.f, 1.f) * 255.f);
        px[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.f, 1.f) * 255.f);
        px[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.f, 1.f) * 255.f);
        px[i * 4 + 3] = 255;
      }
      return renderer.upload_rgba_texture(4, 4, px);
    };

    for (const auto& cand : cands) {
      std::error_code ec;
      if (!fs::is_regular_file(cand, ec)) continue;
      try {
        auto data = bf2::load_obj_file(cand.string());
        if (data.vertices.empty() || data.indices.empty()) continue;

        // Ground lift after Z-up → Y-up so wheels sit on terrain.
        float min_y = 1e9f;
        for (const auto& v : data.vertices) {
          const glm::vec4 p = hawk_mesh_fix * glm::vec4(v.position.x, v.position.y, v.position.z, 1.f);
          min_y = std::min(min_y, p.y);
        }
        hawk_mesh = renderer.upload_textured(data);
        if (hawk_mesh.vao == 0) continue;

        // Bind olive / rocket / tire colours from MTL (FBX had no map_Kd).
        const fs::path mtl_path = cand.parent_path() / "mim23_launcher.mtl";
        auto mats = bf2::load_obj_materials(mtl_path.string());
        // Sensible defaults if MTL missing.
        if (mats.find("Mim23_Hull_Mat") == mats.end())
          mats["Mim23_Hull_Mat"] = {"Mim23_Hull_Mat", {0.42f, 0.48f, 0.32f}};
        if (mats.find("Mim23_Rocket_Mat") == mats.end())
          mats["Mim23_Rocket_Mat"] = {"Mim23_Rocket_Mat", {0.82f, 0.78f, 0.70f}};
        if (mats.find("Mim23_Tire_Mat") == mats.end())
          mats["Mim23_Tire_Mat"] = {"Mim23_Tire_Mat", {0.12f, 0.12f, 0.12f}};
        if (mats.find("Mim23_Tower_Mat") == mats.end())
          mats["Mim23_Tower_Mat"] = {"Mim23_Tower_Mat", {0.55f, 0.52f, 0.38f}};
        for (std::size_t i = 0; i < hawk_mesh.submeshes.size() && i < data.submeshes.size(); ++i) {
          const std::string& mat_name = data.submeshes[i].base_map;
          auto it = mats.find(mat_name);
          const float* kd = it != mats.end() ? it->second.kd : mats["Mim23_Hull_Mat"].kd;
          hawk_mesh.submeshes[i].base_tex = solid_tex(kd[0], kd[1], kd[2]);
        }

        // Open dirt south of the Chinese Z-8 transport heli (-638,-316), off the runway.
        hawk_pos = glm::vec3(-655.f, 0.f, -345.f);
        hawk_pos.y = world.terrain_height(hawk_pos.x, hawk_pos.z);
        hawk_ground_lift = (min_y < 1e8f) ? -min_y : 0.f;
        hawk_present = true;
        hawk_heading_init = 100.f;  // face roughly toward the airfield / runway
        std::cout << "Hawk emplacement: " << cand.string() << " at " << hawk_pos.x << ","
                  << hawk_pos.y << "," << hawk_pos.z << " scale=" << hawk_draw_scale
                  << " lift=" << hawk_ground_lift << " mats=" << hawk_mesh.submeshes.size()
                  << '\n';
        break;
      } catch (const std::exception& ex) {
        std::cerr << "Hawk load failed (" << cand.string() << "): " << ex.what() << '\n';
      }
    }
    if (!hawk_present) std::cout << "Hawk emplacement: not found (optional content)\n";
  }

  {
    const char* tpath = "effects/weapons/tracers/geometry/p_tracer_g/meshes/p_tracer_g.bundledmesh";
    try {
      const auto mesh = resources.load_mesh(tpath);
      const auto data = bf2::MeshLoader::extract_textured(mesh, 0, 0);
      if (!data.vertices.empty()) {
        tracer_mesh = renderer.upload_textured(data);
        for (std::size_t i = 0; i < tracer_mesh.submeshes.size() && i < data.submeshes.size();
             ++i) {
          tracer_mesh.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
        }
        tracer_tex = textures.get("effects/textures/tracer_mg.dds");
        if (!tracer_tex && !data.submeshes.empty())
          tracer_tex = textures.get(data.submeshes[0].base_map);
        std::cout << "Tracer mesh: p_tracer_g\n";
      }
    } catch (...) {
    }
  }

  // Third-person animated soldier: GPU-skinned body driven by 3p locomotion
  // clips (stand / walk / run) selected by movement speed.
  bf2::GpuSkinnedMesh soldier_mesh{};   // player (friendly, US)
  bf2::GpuSkinnedMesh enemy_mesh{};     // opfor body (falls back to soldier_mesh)
  std::uint32_t soldier_tex = 0;
  std::uint32_t enemy_tex = 0;
  bf2::Mesh soldier_src;
  bf2::Mesh enemy_src;  // opfor mesh source (own bind pose for correct skinning)
  bf2::Skeleton soldier_ske;
  dalian::SoldierAnimLibrary soldier_anim_lib;
  dalian::SoldierAnimSet soldier_anims{};
  bool have_soldier = false;
  bool have_enemy_mesh = false;
  bool have_clip_stand = false, have_clip_walk = false, have_clip_run = false;
  dalian::WeaponProfile weapon_profile =
      weapon_index < weapon_profiles.size() ? weapon_profiles[weapon_index] : dalian::WeaponProfile{};
  if (!weapon_profile.valid) {
    for (const auto& wp : weapon_profiles) {
      if (wp.valid) {
        weapon_profile = wp;
        break;
      }
    }
  }
  if (weapon_profile.valid) {
    std::cout << "Weapon profile: " << weapon_profile.name << " rate=" << weapon_profile.fire_rate
              << " dmg=" << weapon_profile.damage << " mag=" << weapon_profile.magazine_size
              << '\n';
  }

  // Pick the diffuse (colour) map from a skinnedmesh material set. BF2 kit
  // materials list several maps; the colour one is usually first and lacks the
  // bump/specular suffixes. Returns a GL texture id (0 if none resolves).
  auto pick_diffuse = [&](const bf2::Mesh& mesh, std::size_t g, std::size_t l) -> std::uint32_t {
    if (g >= mesh.geometries.size() || l >= mesh.geometries[g].lods.size()) return 0;
    auto looks_diffuse = [](std::string s) {
      for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
      return s.find("_b.") == std::string::npos && s.find("bump") == std::string::npos &&
             s.find("normal") == std::string::npos && s.find("_s.") == std::string::npos &&
             s.find("spec") == std::string::npos;
    };
    std::uint32_t first_any = 0;
    for (const auto& mat : mesh.geometries[g].lods[l].materials) {
      for (const auto& mp : mat.maps) {
        if (mp.empty()) continue;
        const std::uint32_t t = textures.get(mp);
        if (t == 0) continue;
        if (first_any == 0) first_any = t;
        if (looks_diffuse(mp)) return t;
      }
    }
    return first_any;
  };

  {
    const char* kSoldierMesh = "soldiers/us/meshes/us_light_soldier.skinnedmesh";
    const char* kSoldierSke = "soldiers/common/animations/3p_setup.ske";
    try {
      const auto mb = resources.read_bytes(kSoldierMesh);
      const auto sb = resources.read_bytes(kSoldierSke);
      if (mb && sb) {
        soldier_src = bf2::MeshLoader::load_from_memory(*mb, bf2::MeshKind::Skinned);
        soldier_ske = bf2::SkeletonLoader::load_from_memory(*sb);
        const auto geom = bf2::extract_skinned(soldier_src, soldier_ske, /*geom=*/1, /*lod=*/0);
        if (!geom.vertices.empty()) {
          soldier_mesh = renderer.upload_skinned(geom);
          have_soldier = soldier_mesh.vao != 0;
          soldier_tex = pick_diffuse(soldier_src, 1, 0);
          // Bind each submesh (body/head/gear) to its own colour map.
          for (std::size_t s = 0;
               s < soldier_mesh.submeshes.size() && s < geom.submeshes.size(); ++s) {
            soldier_mesh.submeshes[s].tex = textures.get(geom.submeshes[s].diffuse_map);
          }
        }
      }
    } catch (const std::exception&) {
      have_soldier = false;
    }
    // Opfor body for enemies: try a few vanilla factions; they share the 3p
    // skeleton, so the same animation palette drives them. Falls back to the US
    // body with a tint if none are present.
    const char* kOpforMeshes[] = {
        "soldiers/mec/meshes/mec_light_soldier.skinnedmesh",
        "soldiers/ch/meshes/ch_light_soldier.skinnedmesh",
        "soldiers/mec/meshes/mec_heavy_soldier.skinnedmesh",
    };
    for (const char* path : kOpforMeshes) {
      try {
        const auto eb = resources.read_bytes(path);
        if (!eb) continue;
        bf2::Mesh esrc = bf2::MeshLoader::load_from_memory(*eb, bf2::MeshKind::Skinned);
        const auto egeom = bf2::extract_skinned(esrc, soldier_ske, /*geom=*/1, /*lod=*/0);
        if (egeom.vertices.empty()) continue;
        enemy_mesh = renderer.upload_skinned(egeom);
        if (enemy_mesh.vao != 0) {
          enemy_tex = pick_diffuse(esrc, 1, 0);
          for (std::size_t s = 0;
               s < enemy_mesh.submeshes.size() && s < egeom.submeshes.size(); ++s) {
            enemy_mesh.submeshes[s].tex = textures.get(egeom.submeshes[s].diffuse_map);
          }
          enemy_src = std::move(esrc);
          have_enemy_mesh = true;
          std::cout << "Enemy body: " << path << '\n';
          break;
        }
      } catch (const std::exception&) {
      }
    }
    if (have_soldier && soldier_anim_lib.load_from_inc(resources)) {
      soldier_anim_lib.fill_anim_set(soldier_anims);
      have_clip_stand = soldier_anims.stand != nullptr;
      have_clip_walk = soldier_anims.walk != nullptr;
      have_clip_run = soldier_anims.run != nullptr;
      std::cout << "Soldier anim library: " << soldier_anim_lib.size() << " clips loaded\n";
    }
    std::cout << "Soldier body: " << (have_soldier ? "US light (3p, animated)" : "none")
              << ", tex " << soldier_tex << "; enemy tex " << enemy_tex << '\n';
    std::cout << "Clips: stand=" << (soldier_anims.stand ? soldier_anims.stand->frame_count : -1)
              << " walk=" << (soldier_anims.walk ? soldier_anims.walk->frame_count : -1)
              << " run=" << (soldier_anims.run ? soldier_anims.run->frame_count : -1)
              << " (ske nodes=" << soldier_ske.nodes.size() << ")\n";
  }

  // ---- Authoritative game simulation (headless-tickable, net-ready) ----
  {
    dalian::SimInitParams sim_init;
    sim_init.world = &world;
    sim_init.water_y = atmo.has_water ? atmo.water_level : -1e9f;
    sim_init.spawn = {spawn.x, spawn.y, spawn.z};
    sim_init.control_points = control_points;
    sim_init.soldier_ske = &soldier_ske;
    sim_init.clip_stand = soldier_anims.stand;
    sim_init.clip_walk = soldier_anims.walk;
    sim_init.clip_run = soldier_anims.run;
    sim_init.have_soldier = have_soldier;
    sim_init.have_clip_stand = have_clip_stand;
    sim_init.have_clip_walk = have_clip_walk;
    sim_init.have_clip_run = have_clip_run;
    sim_init.nav_mesh = nav_mesh.valid() ? &nav_mesh : nullptr;
    sim_init.weapon = weapon_profile;
    sim_init.sam_missile = dalian::load_projectile_profile(resources, "igla_9k38");
    sim_init.at_missile = dalian::load_projectile_profile(resources, "insgr_rpg");
    if (!sim_init.at_missile.valid) {
      sim_init.at_missile = dalian::load_projectile_profile(resources, "at_predator");
    }
    sim_init.enemy_weapon =
        dalian::load_weapon_profile(resources, "weapons/handheld/rurif_ak47/rurif_ak47.tweak");
    if (sim_init.enemy_weapon.valid) {
      sim_init.enemy_weapon.name = "AK-47";
    } else {
      sim_init.enemy_weapon =
          dalian::load_weapon_profile(resources, "weapons/handheld/usrif_m16a2/usrif_m16a2.tweak");
      if (sim_init.enemy_weapon.valid) sim_init.enemy_weapon.name = "M16A2";
    }
    sim_init.soldier_anims = soldier_anims;
    sim_init.missile_headless_demo = shot_missile;
    dalian::GameLogicDefaults game_logic{};
    if (!objects_zip.empty()) {
      const auto mod_dir = std::filesystem::path(objects_zip).parent_path();
      if (std::ifstream in(mod_dir / "GameLogicInit.con"); in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        game_logic = dalian::parse_game_logic_init(text);
      }
      if (std::ifstream in(mod_dir / "Settings" / "ServerSettings.con"); in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto srv = dalian::parse_server_settings(text);
        if (srv.valid) game_logic.ticket_ratio_percent = srv.ticket_ratio_percent;
      }
      if (game_logic.valid) {
        std::cout << "GameLogic: tickets T1=" << game_logic.tickets_team1 << " T2="
                  << game_logic.tickets_team2
                  << " ratio=" << game_logic.ticket_ratio_percent << "%\n";
      }
    }
    const int base_tickets =
        game_logic.valid ? std::max(game_logic.tickets_team1, game_logic.tickets_team2) : 250;
    sim_init.starting_tickets =
        std::max(1, base_tickets * game_logic.ticket_ratio_percent / 100);
    sim_init.map_layout = map_layout;
    sim_init.team1_faction_id = map_layout.team1_faction_id;
    sim_init.team2_faction_id = map_layout.team2_faction_id;
    if (session_mp.enabled) {
      sim_init.bots_enabled = session_mp.bots_enabled;
      sim_init.bot_count = session_mp.bot_count;
      sim_init.bot_difficulty = session_mp.bot_difficulty;
    } else {
      sim_init.bots_enabled = settings.mp_bots_enabled;
      sim_init.bot_count = settings.mp_bot_count;
      sim_init.bot_difficulty = settings.mp_bot_difficulty;
    }
    sim_init.multiplayer = session_mp.enabled;
    sim_init.bot_names = dalian::load_bot_names(bf2_root);
    sim_init.player_label = settings.player_name.empty() ? "You" : settings.player_name;
    sim_init.weapon.name = weapon_defs[weapon_index].name;
    game_sim.init(sim_init);
    if (weapon_profile.valid) game_sim.set_weapon_profile(weapon_profile, true);
    game_sim.state().vehicles = std::move(loaded_vehicles);
    std::cout << "Enemies: " << game_sim.state().enemies.size() << " defenders\n";
    std::cout << "Conquest: " << game_sim.state().control_points.size() << " control points, "
              << sim_init.starting_tickets << " tickets per team\n";
    load_step(0.88f, "FINALIZING", "Soldier animations, audio, and conquest setup...");
  }
  load_step(1.f, "MAP LOADED", session_mp.enabled ? "Multiplayer session connected."
                                                   : "All assets loaded.");
  if (!headless_shot) {
    loading_scr.wait_until_ready(
        window, renderer,
        session_mp.enabled ? "Deploy when your squad is ready."
                           : "Listen to the track, then press Ready to open deployment.");
    if (have_game_audio) {
      if (const auto amb = resources.read_bytes("ambientobjects.con")) {
        game_audio.start_level_ambient(
            resources, std::string(reinterpret_cast<const char*>(amb->data()), amb->size()));
      }
    }
  }
  dalian::GameState& G = game_sim.state();
  auto& vehicles = G.vehicles;
  auto& enemies = G.enemies;
  auto& tracers = G.tracers;
  auto& impacts = G.impacts;
  auto& projectiles = G.projectiles;
  auto& missiles = G.missiles;
  auto& smoke = G.smoke;
  auto& explosions = G.explosions;
  auto& flares = G.flares;
  int& in_vehicle = G.in_vehicle;
  int& player_seat = G.player_seat;
  float& air_pitch_stick = G.air_pitch_stick;
  float& air_roll_stick = G.air_roll_stick;
  float& air_input_grace = G.air_input_grace;
  float& fire_cooldown = G.fire_cooldown;
  float& muzzle_flash = G.muzzle_flash;
  float& recoil = G.recoil;
  bool& ballistic = G.ballistic;
  float& missile_reload = G.missile_reload;
  float& heli_rocket_cd = G.heli_rocket_cd;
  float& heli_gun_cd = G.heli_gun_cd;
  float& heli_grocket_cd = G.heli_grocket_cd;
  float& heli_flare_cd = G.heli_flare_cd;
  int& gunner_target = G.gunner_target;
  float& gunner_acquire = G.gunner_acquire;
  bool& gunner_engaging = G.gunner_engaging;
  int& player_kills = G.player_kills;
  int& player_deaths = G.player_deaths;
  float& player_health = G.player_health;
  float& player_regen_delay = G.player_regen_delay;
  float& player_stamina = G.player_stamina;
  int& mag_ammo = G.mag_ammo;
  int& reserve_ammo = G.reserve_ammo;
  bool& reloading = G.reloading;
  float& reload_timer = G.reload_timer;
  glm::vec3& wind_base = G.wind_base;
  glm::vec3& wind = G.wind;
  auto& conquest_points = G.control_points;
  auto& tickets = G.tickets;
  bool& match_over = G.match_over;
  bool& match_started = G.match_started;
  dalian::TeamId& player_team = G.player_team;
  int& team1_faction_id = G.team1_faction_id;
  int& team2_faction_id = G.team2_faction_id;
  dalian::TeamId& winning_team = G.winning_team;
  float& round_time = G.round_time;

  bool third_person = false;
  float anim_time = 0.f;
  std::uint32_t net_input_seq = 0;
  double net_last_send_at = -1.0;
  dalian::PlayerInput last_net_inp{};
  bool air_stick_moved = false; // set when mouse moves the flight stick this frame
  float air_mouse_dx_accum = 0.f;
  float air_mouse_dy_accum = 0.f;
  float air_throttle_wheel_accum = 0.f;
  auto air_invert_fn = [&]() { return settings.invert_air; };

  constexpr float kMuzzleSpeed = 340.f;
  constexpr float kBulletGravity = -9.81f;
  constexpr float kBulletLife = 3.0f;
  constexpr float kBulletDrag = 0.0011f;

  const float water_y = atmo.has_water ? atmo.water_level : -1e9f;
  auto frand = []() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); };
  auto ground_surface = [&](float x, float z, float refy) -> float {
    const float terr = world.terrain_height(x, z);
    if (refy > terr + 1.0f) {
      const auto dn = world.raycast({x, refy + 8.f, z}, {0.f, -1.f, 0.f}, 60.f);
      if (dn.hit && std::fabs(dn.normal.y) > 0.4f && dn.point.y > terr + 1.0f &&
          dn.point.y <= refy + 8.5f) {
        return dn.point.y;
      }
    }
    return terr;
  };

  using EnemyHit = dalian::EnemyHit;
  auto shoot_enemies = [&](const glm::vec3& o, const glm::vec3& dir, float maxd) {
    return game_sim.raycast_enemies(o, dir, maxd);
  };
  auto damage_enemy = [&](int idx, int zone) { game_sim.apply_enemy_damage(idx, zone); };
  auto explode_at = [&](const glm::vec3& center, float radius, float max_damage) {
    game_sim.apply_explosion(center, radius, max_damage);
  };

  float yaw = -90.f;    // looking toward -Z
  float pitch = -5.f;
  float sensitivity = settings.mouse_sensitivity;

  // Diagnostic: BF2_VEHCAM=<substr> teleports the player next to the first vehicle
  // whose mesh path contains <substr> and aims at it (for headless verification).
  if (const char* vc = std::getenv("BF2_VEHCAM")) {
    std::string want = vc;
    for (auto& c : want) c = static_cast<char>(std::tolower((unsigned char)c));
    for (const auto& v : vehicles) {
      std::string k = v.mesh_key;
      for (auto& c : k) c = static_cast<char>(std::tolower((unsigned char)c));
      if (k.find(want) != std::string::npos) {
        const float ds = std::getenv("BF2_VEHDIST") ? std::atof(std::getenv("BF2_VEHDIST")) : 1.f;
        const glm::vec3 off =
            (v.is_air ? glm::vec3(11.f, 3.f, -17.f) : glm::vec3(9.f, 2.5f, 9.f)) * ds;
        const glm::vec3 aim = v.is_air ? v.origin + glm::vec3(0.f, 1.2f, -4.f) : v.origin;
        std::cout << "VEHCAM origin " << v.origin.x << " " << v.origin.y << " " << v.origin.z
                  << " air=" << v.is_air << "\n";
        player.position = {v.origin.x + off.x, v.origin.y + off.y + player.eye_height,
                           v.origin.z + off.z};
        const glm::vec3 to = aim - glm::vec3(player.position.x, player.position.y,
                                                  player.position.z);
        yaw = glm::degrees(std::atan2(to.z, to.x));
        pitch = glm::degrees(std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z)));
        std::cout << "VEHCAM at " << v.mesh_key << " parts=" << v.parts.size() << '\n';
        if (std::getenv("BF2_VEHENTER")) in_vehicle = static_cast<int>(&v - vehicles.data());
        break;
      }
    }
  }

  // Diagnostic: BF2_ENEMYCAM=<index> frames one enemy from a raised third-person
  // angle (for headless verification of enemy models/textures/animation). With no
  // index it picks the enemy nearest the spawn, which is usually out in the open.
  if (const char* ec = std::getenv("BF2_ENEMYCAM"); ec && !enemies.empty()) {
    std::size_t idx = 0;
    if (ec[0] && std::isdigit((unsigned char)ec[0])) {
      idx = std::min(static_cast<std::size_t>(std::atoi(ec)), enemies.size() - 1);
    } else {
      float best = 1e30f;
      for (std::size_t i = 0; i < enemies.size(); ++i) {
        const float dx = enemies[i].pos.x - spawn.x, dz = enemies[i].pos.z - spawn.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 < best) { best = d2; idx = i; }
      }
    }
    const glm::vec3 e = enemies[idx].pos;
    player.position = {e.x + 2.6f, e.y + 1.5f, e.z + 2.6f};
    const glm::vec3 to = e + glm::vec3(0.f, 1.0f, 0.f) -
                         glm::vec3(player.position.x, player.position.y, player.position.z);
    yaw = glm::degrees(std::atan2(to.z, to.x));
    pitch = glm::degrees(std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z)));
    std::cout << "ENEMYCAM #" << idx << " at " << e.x << ',' << e.z << " of " << enemies.size()
              << " enemies\n";
  }

  SDL_SetRelativeMouseMode(SDL_TRUE);

  bool running = true;
  bool pause_open = false;
  bool scoreboard_open = false;
  dalian::InputRouter input;
  bool rmb_was_down = false;

  // ---- Multiplayer session (milestone 1: players walking/shooting in sync) ---
  std::unique_ptr<bf2::Net> owned_net = std::move(session_net);
  if (!owned_net) owned_net = std::make_unique<bf2::Net>();
  bf2::Net& net = *owned_net;
  if (session_mp.enabled) {
    net.set_lobby_mode(false);
    std::cout << "Net: multiplayer session"
              << (session_mp.is_host ? " (host)\n" : " (client)\n");
  } else if (net_mode == 1) {
    if (net.host(net_port, net_dedicated))
      std::cout << "Net: hosting on port " << net_port << (net_dedicated ? " (dedicated)\n" : "\n");
    else
      std::cerr << "Net: failed to host on port " << net_port << '\n';
  } else if (net_mode == 2) {
    if (net.connect(net_connect, net_port))
      std::cout << "Net: connecting to " << net_connect << ':' << net_port << "...\n";
    else
      std::cerr << "Net: failed to start client for " << net_connect << ':' << net_port << '\n';
  }
  if (session_mp.enabled && net.active()) {
    net.set_faction(static_cast<std::uint16_t>(session_mp.faction_id));
  }

  const bool advertise_session = net.is_server() && (session_mp.enabled || net_mode == 1);
  dalian::DiscoveryHost session_discovery;
  if (advertise_session) {
    const std::uint16_t adv_port =
        session_mp.enabled ? session_mp.port : net_port;
    session_discovery.start(adv_port);
    const std::string ts_subnet =
        settings.tailscale_subnet.empty() ? dalian::detect_tailscale_subnet()
                                        : settings.tailscale_subnet;
    session_discovery.set_broadcast_targets(true, settings.use_tailscale, ts_subnet);
  }

  Uint64 prev = SDL_GetPerformanceCounter();
  // Object draw distance (tunable live from the pause/options menu).
  float draw_dist = settings.draw_distance;
  float draw_dist2 = draw_dist * draw_dist;

  // FPV drone: a 6-DoF quadcopter you can launch and fly. Mouse steers pitch/
  // roll (self-centring, acro style), A/D yaw, W/S throttle. F9 = recon toggle.
  // F10 = kamikaze loitering munition (one-way, detonates on impact / LMB / Space).
  // F8 = car-launched SAM: opens tactical map; click destination to fire (missile leaves the car).
  bf2::DroneController drone;
  bool drone_mode = false;
  bool kamikaze_mode = false;
  glm::vec3 drone_prev_pos{};

  // Cascaded Shadow Maps: cascades are refit to the live camera frustum each
  // frame (splits + light-space matrices). The depth-pass rendering is the next
  // integration step; the matrices are already correct and inspectable.
  bf2::CascadedShadowMaps<4> csm;
  float drone_throttle = 0.f;
  float drone_stick_pitch = 0.f;  // per-frame mouse-driven commands
  float drone_stick_roll = 0.f;
  float signal = 1.f;  // FPV link quality 0..1 (drops with range / occlusion)
  float sam_hud_flash = 0.f;  // brief "SAM LAUNCH" cue after map fire
  bool sam_map_open = false;
  glm::vec3 sam_pending_target{0.f};
  bool sam_has_pending_target = false;
  int sam_launch_mode = 0;  // 0 = car, 1 = hawk emplacement
  int hawk_ammo = 3;
  float hawk_heading = hawk_heading_init;  // yaw toward last / pending target
  float hawk_aim_pitch = 25.f;
  auto detonate_kamikaze = [&](const glm::vec3& pt) {
    explode_at(pt, 6.f, 130.f);
    drone_mode = false;
    kamikaze_mode = false;
  };

  Uint64 title_timer = 0;
  int cur_w = settings.width;
  int cur_h = settings.height;
  dalian::sync_drawable_size(window, cur_w, cur_h);

  // Grass geometry is rebuilt only when the player moves far enough, so it stays
  // stable and cheap. Extended well beyond the 2004 ViewDistance for a lush
  // near-field on modern hardware; BF2_GRASSDIST tunes it.
  std::vector<float> grass_verts;
  glm::vec2 grass_center(1e9f, 1e9f);
  float grass_radius = settings.grass_distance;
  if (const char* gd = std::getenv("BF2_GRASSDIST"))
    grass_radius = std::max(10.f, static_cast<float>(std::atof(gd)));

  int frame_no = 0;
  if (shot_third_person && have_soldier) third_person = true;
  if (shot_missile) {  // face the open field so the missile flight stays in frame
    yaw = -158.f;
    pitch = 3.f;
  }
  if (shot_drone) {  // headless drone screenshot: launch immediately
    drone_mode = true;
    drone = bf2::DroneController{};
    drone.position = glm::vec3(player.position.x, player.position.y + 8.f, player.position.z);
    drone.velocity = glm::vec3(0.f);
    drone_throttle = 0.30f;
    signal = 1.f;
  }

  // ===== BF2-style class loadout, spawn selection & gadgets =================
  int player_faction_id =
      session_mp.enabled ? session_mp.faction_id : settings.default_faction;
  // Each map has two geographic teams (Side 1 / Side 2) with their own spawn sets.
  // Before the match, assign any army to each side, then choose which side you play.
  dalian::TeamId fight_for_team = dalian::TeamId::Team1;
  if (player_faction_id == team2_faction_id) {
    fight_for_team = dalian::TeamId::Team2;
  } else if (player_faction_id != team1_faction_id) {
    team1_faction_id = player_faction_id;
    if (team1_faction_id == team2_faction_id) team2_faction_id = 3;
    fight_for_team = dalian::TeamId::Team1;
  }
  game_sim.set_match_factions(team1_faction_id, team2_faction_id, fight_for_team);
  player_team = fight_for_team;
  player_faction_id =
      player_team == dalian::TeamId::Team1 ? team1_faction_id : team2_faction_id;
  int enemy_faction_id =
      player_team == dalian::TeamId::Team1 ? team2_faction_id : team1_faction_id;
  float deploy_side1_scroll = 0.f;
  float deploy_side2_scroll = 0.f;

  auto map_side_label = [&](dalian::TeamId side) -> std::string {
    for (const auto& cp : map_layout.control_points) {
      if (cp.initial_team == side) return cp.label;
    }
    return side == dalian::TeamId::Team1 ? "Side 1" : "Side 2";
  };
  const std::string side1_home = map_side_label(dalian::TeamId::Team1);
  const std::string side2_home = map_side_label(dalian::TeamId::Team2);

  struct KitDef {
    const char* name;
    int weapon[2];  // weapon_defs index for [USMC, MEC]
    int grenades;
    int c4;
    bool medic;   // carries a medkit (self-heal)
    bool at;      // anti-tank rocket launcher
    const char* gadget;
  };
  // Weapon indices: 0=M4 1=M16A2 2=AK-47 3=AK-101 4=G36E 5=G3A3.
  const std::vector<KitDef> kits = {
      {"Assault", {1, 2}, 3, 0, false, false, "Frag Grenades x3"},
      {"Special Forces", {0, 3}, 1, 3, false, false, "C4 Charges x3"},
      {"Medic", {4, 3}, 1, 0, true, false, "Medkit (H to heal)"},
      {"Engineer", {5, 2}, 1, 0, false, false, "Repair Tool"},
      {"Support", {1, 2}, 2, 0, false, false, "Ammo Supply"},
      {"Anti-Tank", {0, 2}, 1, 0, false, true, "AT Rocket (RMB)"},
      {"Sniper", {5, 3}, 1, 1, false, false, "Claymore x1"},
  };

  const int kMagSize = 30;
  const int kReserveMags = 6;
  mag_ammo = kMagSize;
  reserve_ammo = kMagSize * kReserveMags;
  player_stamina = 100.f;
  reload_timer = 0.f;
  reloading = false;
  int grenades_left = 0;
  int c4_left = 0;
  bool has_medkit = false;
  bool has_at = false;
  float medkit_cd = 0.f;

  int selected_kit = 0;  // highlighted in the deploy screen
  int player_kit = 0;    // active kit after deploy

  // Deploy map: one marker per capture point (BF2 shows flags, not every soldier spawn).
  struct DeployMarker {
    std::string name;
    glm::vec3 map_pos;
    glm::vec3 deploy_pos;
    int bf2_cp_id = 0;
  };
  std::vector<DeployMarker> deploy_markers;
  {
    std::unordered_map<int, std::size_t> cp_index;
    for (const auto& cp : map_layout.control_points) {
      cp_index[cp.bf2_id] = deploy_markers.size();
      deploy_markers.push_back({cp.label, cp.pos, cp.pos, cp.bf2_id});
    }
    for (const auto& sp : map_layout.soldier_spawns) {
      const auto it = cp_index.find(sp.bf2_cp_id);
      if (it == cp_index.end()) continue;
      DeployMarker& m = deploy_markers[it->second];
      if (m.deploy_pos == m.map_pos) m.deploy_pos = sp.pos;
    }
    if (deploy_markers.empty()) {
      static const char* phon[] = {"ALPHA",   "BRAVO", "CHARLIE", "DELTA", "ECHO", "FOXTROT",
                                   "GOLF",    "HOTEL", "INDIA",   "JULIET", "KILO", "LIMA"};
      std::vector<glm::vec3> chosen;
      for (const auto& cp : control_points) {
        bool too_close = false;
        for (const auto& p : chosen)
          if (glm::distance(glm::vec2(cp.x, cp.z), glm::vec2(p.x, p.z)) < 55.f) {
            too_close = true;
            break;
          }
        if (too_close) continue;
        chosen.push_back(cp);
        if (chosen.size() >= 12) break;
      }
      if (chosen.empty()) chosen.push_back(spawn);
      for (std::size_t i = 0; i < chosen.size(); ++i)
        deploy_markers.push_back({phon[i % 12], chosen[i], chosen[i], 0});
    }
  }
  int selected_spawn = 0;
  auto spawn_point_owned = [&](std::size_t idx) -> bool {
    if (idx >= deploy_markers.size()) return false;
    const auto& m = deploy_markers[idx];
    if (m.bf2_cp_id != 0) return game_sim.can_team_spawn_at_cp(m.bf2_cp_id, player_team);
    return game_sim.can_team_spawn_at(m.deploy_pos, player_team);
  };
  auto pick_spawnable = [&]() {
    for (std::size_t i = 0; i < deploy_markers.size(); ++i) {
      if (spawn_point_owned(i)) {
        selected_spawn = static_cast<int>(i);
        return;
      }
    }
  };
  {
    float best = 1e30f;
    for (std::size_t i = 0; i < deploy_markers.size(); ++i) {
      const float d = glm::distance(glm::vec2(deploy_markers[i].deploy_pos.x,
                                              deploy_markers[i].deploy_pos.z),
                                    glm::vec2(spawn.x, spawn.z));
      if (d < best) {
        best = d;
        selected_spawn = static_cast<int>(i);
      }
    }
    if (!spawn_point_owned(static_cast<std::size_t>(selected_spawn))) pick_spawnable();
  }

  auto sync_player_from_sides = [&]() {
    player_faction_id =
        player_team == dalian::TeamId::Team1 ? team1_faction_id : team2_faction_id;
    enemy_faction_id =
        player_team == dalian::TeamId::Team1 ? team2_faction_id : team1_faction_id;
    game_sim.set_match_factions(team1_faction_id, team2_faction_id, player_team);
    pick_spawnable();
  };

  // Thrown/placed gadgets.
  struct Grenade {
    glm::vec3 pos;
    glm::vec3 vel;
    float fuse;
  };
  std::vector<Grenade> live_grenades;
  std::vector<glm::vec3> placed_c4;

  bool deploy_open = shot_path.empty();  // headless captures skip the spawn menu
  if (std::getenv("BF2_DEPLOYSHOT")) deploy_open = true;  // force menu for capture
  bool deploy_click = false;

  // Push an (x,z) point out of any vehicle hull it overlaps, treating each
  // vehicle as an oriented box (its local right/forward axes). `feet_y` is used
  // to skip vehicles the point is clearly above/below (so you can stand on a
  // flatbed). Returns true if it moved the point. `ignore` skips one vehicle
  // (the one you're driving). Shared by walking collision and spawn placement.
  auto push_out_of_vehicles = [&](float& x, float& z, float feet_y, int ignore) -> bool {
    bool moved = false;
    for (std::size_t vi = 0; vi < vehicles.size(); ++vi) {
      if (static_cast<int>(vi) == ignore) continue;
      const Vehicle& v = vehicles[vi];
      if (v.mesh_key.find("vehicles/") == std::string::npos) continue;
      // Vertical gate: ignore if the player's feet are above the hull top or a
      // couple metres below its base (prevents fighting with the support pass).
      const float base = v.pos.y - v.clearance;
      if (feet_y > base + v.col_half.y * 2.f + 0.3f || feet_y < base - 2.0f) continue;
      const float hd = glm::radians(v.heading);
      const glm::vec3 fwd(std::sin(hd), 0.f, std::cos(hd));
      const glm::vec3 rgt(fwd.z, 0.f, -fwd.x);
      const float dx = x - v.pos.x, dz = z - v.pos.z;
      // Project into the box's local frame.
      float lr = dx * rgt.x + dz * rgt.z;   // along right axis
      float lf = dx * fwd.x + dz * fwd.z;   // along forward axis
      const float pr = 0.4f;                // player radius padding
      const float ex = v.col_half.x + pr, ez = v.col_half.z + pr;
      if (std::fabs(lr) >= ex || std::fabs(lf) >= ez) continue;  // outside box
      // Inside: push out along the axis of least penetration.
      const float penR = ex - std::fabs(lr);
      const float penF = ez - std::fabs(lf);
      if (penR < penF) {
        lr += (lr >= 0.f ? penR : -penR);
      } else {
        lf += (lf >= 0.f ? penF : -penF);
      }
      x = v.pos.x + rgt.x * lr + fwd.x * lf;
      z = v.pos.z + rgt.z * lr + fwd.z * lf;
      moved = true;
    }
    return moved;
  };

  // Find a clear standing position near `desired`: nudge out of vehicles and, if
  // the point is boxed in by building geometry (walls on all sides / no sky),
  // spiral-search outward for an open spot. Prevents spawning inside a hull or a
  // sealed building you can't escape.
  // Standing surface at (x,z): the terrain, unless a solid deck/bridge/floor sits
  // above it near the reference height `refy` (e.g. an aircraft-carrier deck). We
  // probe downward from just above the reference so carrier control points spawn
  // you on the flight deck instead of the sea floor far below.
  auto surface_height = [&](float x, float z, float refy) -> float {
    const float terr = world.terrain_height(x, z);
    const auto dn = world.raycast({x, refy + 8.f, z}, {0.f, -1.f, 0.f}, 60.f);
    if (dn.hit && std::fabs(dn.normal.y) > 0.4f && dn.point.y > terr + 1.0f &&
        dn.point.y <= refy + 8.5f) {
      return dn.point.y;
    }
    return terr;
  };
  auto find_safe_spawn = [&](const glm::vec3& desired) -> glm::vec3 {
    const float refy = desired.y;  // control-point height carries the deck level
    auto trapped = [&](float x, float z, float& out_y) -> bool {
      const float feet = surface_height(x, z, refy);
      out_y = feet;
      // Below the sea surface? Never a valid standing spot — force the search to
      // keep looking for dry ground so the player never spawns underwater.
      if (feet < water_y - 0.3f) return true;
      // Blocked overhead within head height? (inside a building/under a hull.)
      const auto up = world.raycast({x, feet + 0.2f, z}, {0.f, 1.f, 0.f}, 3.0f);
      const bool roofed = up.hit && up.distance < 2.6f;
      // Count how many horizontal directions hit a near wall; boxed-in = trapped.
      int walls = 0;
      const float chest = feet + 1.0f;
      for (int a = 0; a < 8; ++a) {
        const float ang = a * 0.7853981f;
        const glm::vec3 d(std::cos(ang), 0.f, std::sin(ang));
        const auto h = world.raycast({x, chest, z}, {d.x, d.y, d.z}, 1.2f);
        if (h.hit && std::fabs(h.normal.y) < 0.6f) ++walls;
      }
      return roofed && walls >= 5;
    };
    float x = desired.x, z = desired.z, y = 0.f;
    push_out_of_vehicles(x, z, surface_height(x, z, refy), -1);
    if (!trapped(x, z, y)) return {x, surface_height(x, z, refy), z};
    // Spiral outward to find open ground.
    for (float r = 3.f; r <= 40.f; r += 3.f) {
      for (int a = 0; a < 12; ++a) {
        const float ang = a * 0.5235987f;
        float cx = desired.x + std::cos(ang) * r;
        float cz = desired.z + std::sin(ang) * r;
        push_out_of_vehicles(cx, cz, surface_height(cx, cz, refy), -1);
        if (!trapped(cx, cz, y)) return {cx, surface_height(cx, cz, refy), cz};
      }
    }
    // Give up: at least not in a vehicle, and never below the sea surface.
    float fy = surface_height(x, z, refy);
    if (fy < water_y) fy = water_y;
    return {x, fy, z};
  };

  // Apply the chosen kit + spawn: place the player, load the weapon, refill ammo.
  auto apply_loadout = [&]() {
    player_kit = selected_kit;
    const KitDef& k = kits[player_kit];
    const int wf = dalian::faction_kit_side(player_faction_id);
    std::size_t wi = static_cast<std::size_t>(k.weapon[wf]);
    if (wi >= weapon_defs.size() || !load_weapon(wi)) {
      for (std::size_t i = 0; i < weapon_defs.size(); ++i)
        if (load_weapon(i)) {
          wi = i;
          break;
        }
    }
    weapon_index = wi;
    const dalian::WeaponProfile& wprof =
        wi < weapon_profiles.size() ? weapon_profiles[wi] : dalian::WeaponProfile{};
    const int mag = wprof.magazine_size > 0 ? wprof.magazine_size : kMagSize;
    const int reserve = wprof.reserve_ammo > 0 ? wprof.reserve_ammo : mag * kReserveMags;
    mag_ammo = mag;
    reserve_ammo = reserve;
    grenades_left = k.grenades;
    c4_left = k.c4;
    has_medkit = k.medic;
    has_at = k.at;
    reloading = false;
    reload_timer = 0.f;
    if (!deploy_markers.empty()) {
      if (!spawn_point_owned(static_cast<std::size_t>(selected_spawn))) pick_spawnable();
      if (spawn_point_owned(static_cast<std::size_t>(selected_spawn))) {
        const glm::vec3 safe = find_safe_spawn(deploy_markers[selected_spawn].deploy_pos);
        player.position = {safe.x, safe.y + player.eye_height, safe.z};
        world.snap_character_to_ground(player);
        player.vertical_velocity = 0.f;
      }
    }
    player_health = 100.f;
    player_stamina = 100.f;
    live_grenades.clear();
    placed_c4.clear();
    if (wprof.valid) game_sim.set_weapon_profile(wprof, false);
    if (k.at) {
      const bool us_faction = player_faction_id == 0 || player_faction_id == 1;
      const char* at_tpl = us_faction ? "at_predator" : "insgr_rpg";
      dalian::ProjectileProfile atp = dalian::load_projectile_profile(resources, at_tpl);
      if (!atp.valid) {
        atp = dalian::load_projectile_profile(resources, us_faction ? "insgr_rpg" : "at_predator");
      }
      if (atp.valid) game_sim.set_at_missile_profile(atp);
    }
  };
  apply_loadout();  // sensible defaults so the weapon/ammo exist behind the menu
  {
    const float feet = player.position.y - player.eye_height;
    const float terr = world.terrain_height(player.position.x, player.position.z);
    const float support =
        world.support_height(player.position.x, player.position.z, feet + 0.7f, 0.7f);
    std::cout << "Spawn ground: feet=" << feet << " terrain=" << terr << " support=" << support
              << " patches="
              << (level.has_heightmap_cluster ? level.heightmap_cluster.patches().size() : 0)
              << " collision_tris=" << world.collision_triangle_count() << '\n';
    if (std::fabs(feet - support) > 2.f) {
      std::cerr << "WARNING: spawn is not on walkable ground — set Options -> BF2 install path "
                   "and verify HeightmapCluster loaded in the console log\n";
    }
  }
  if (!deploy_open) game_sim.begin_match();

  std::string map_label = std::filesystem::path(archive_path).parent_path().filename().string();
  for (char& c : map_label)
    if (c == '_') c = ' ';

  auto rect_hit = [&](int mx, int my, float x, float y, float w, float h) {
    return dalian::ui_hit(renderer, mx, my, x, y, w, h);
  };

  // Draws the deploy screen and (when clicked) handles kit/spawn/deploy picks.
  auto run_deploy_ui = [&](int mx, int my, bool clicked) {
    constexpr float W = 1600.f, H = 900.f;
    constexpr float M = 28.f;  // outer margin
    renderer.ui_rect(0, 0, W, H, 0.04f, 0.05f, 0.07f, 0.92f);

    const auto& fac_you = dalian::faction_at(player_faction_id);
    const auto& fac_en = dalian::faction_at(enemy_faction_id);

    // ---- Header (reserved band — columns start below this) ----
    const float header_h = 96.f;
    renderer.ui_rect(0, 0, W, header_h, 0.06f, 0.07f, 0.09f, 0.98f);
    renderer.ui_rect(0, header_h - 2.f, W, 2.f, 0.25f, 0.45f, 0.65f, 0.85f);
    renderer.ui_text(M, 22.f, 2.8f, "DEPLOYMENT", 0.94f, 0.96f, 0.98f, 1.f);
    char matchup[160];
    std::snprintf(matchup, sizeof(matchup), "%s  vs  %s", fac_you.country, fac_en.country);
    dalian::draw_clipped_text(renderer, M, 58.f, W * 0.55f, 1.7f, matchup, 0.55f, 0.78f, 1.0f, 1.f);
    dalian::draw_clipped_text(renderer, M, 82.f, W * 0.55f, 1.15f, map_label.c_str(), 0.58f, 0.6f,
                              0.65f, 1.f);

    // Column geometry
    const float content_y = header_h + 16.f;
    const float content_h = H - content_y - M;
    const float col_gap = 16.f;
    const float left_w = 268.f;
    const float mid_w = 268.f;
    const float left_x = M;
    const float mid_x = left_x + left_w + col_gap;
    const float map_x = mid_x + mid_w + col_gap;
    const float map_w = W - map_x - M;
    const float btn_h = 52.f;
    const float map_h = content_h - btn_h - 14.f;

    auto panel = [&](float x, float y, float w, float h) {
      renderer.ui_rect(x, y, w, h, 0.07f, 0.08f, 0.10f, 0.96f);
      renderer.ui_rect(x, y, w, 2.f, 0.22f, 0.38f, 0.55f, 0.7f);
    };
    auto section_label = [&](float x, float y, const char* text) {
      renderer.ui_text(x, y, 1.25f, text, 0.62f, 0.68f, 0.76f, 1.f);
    };

    // ---- Left: armies + fight-for ----
    panel(left_x, content_y, left_w, content_h);
    const float lx = left_x + 12.f;
    const float lw = left_w - 24.f;
    float ly = content_y + 14.f;
    const float frow = 26.f;
    const float list_h = 118.f;

    auto draw_side_army_picker = [&](float x, float y, float w, float h, float& scroll,
                                     int selected_id, const char* title, const char* home,
                                     auto on_pick) {
      section_label(x, y, title);
      char home_line[96];
      std::snprintf(home_line, sizeof(home_line), "Home: %s", home);
      dalian::draw_clipped_text(renderer, x, y + 18.f, w, 1.0f, home_line, 0.48f, 0.58f, 0.72f,
                                0.95f);
      const float list_y = y + 38.f;
      renderer.ui_rect(x, list_y, w, h, 0.04f, 0.05f, 0.06f, 0.98f);
      scroll = std::clamp(scroll, 0.f,
                          std::max(0.f, static_cast<float>(dalian::faction_count()) * frow - h));
      const int start = static_cast<int>(scroll / frow);
      for (std::size_t i = start; i < dalian::faction_count() &&
                                  static_cast<int>(i) < start + static_cast<int>(h / frow) + 2;
           ++i) {
        const float ry = list_y + static_cast<float>(i) * frow - scroll;
        if (ry < list_y - 1.f || ry > list_y + h - frow + 1.f) continue;
        const bool sel = static_cast<int>(i) == selected_id;
        const bool hov = rect_hit(mx, my, x, ry, w, frow);
        renderer.ui_rect(x + 2.f, ry + 1.f, w - 4.f, frow - 3.f, sel ? 0.14f : (hov ? 0.10f : 0.06f),
                         sel ? 0.26f : (hov ? 0.13f : 0.08f), sel ? 0.42f : (hov ? 0.18f : 0.10f),
                         0.98f);
        if (sel) renderer.ui_rect(x + 2.f, ry + 1.f, 3.f, frow - 3.f, 0.35f, 0.7f, 1.f, 1.f);
        const auto& fd = dalian::faction_at(static_cast<int>(i));
        dalian::draw_clipped_text(renderer, x + 12.f, ry + 5.f, w - 20.f, 1.15f, fd.country, 0.9f,
                                  0.92f, 0.95f, 1.f);
        if (clicked && hov) on_pick(static_cast<int>(i));
      }
      return list_y + h;
    };

    float after_s1 =
        draw_side_army_picker(lx, ly, lw, list_h, deploy_side1_scroll, team1_faction_id, "SIDE 1 ARMY",
                              side1_home.c_str(), [&](int fid) {
                                if (fid == team2_faction_id) return;
                                team1_faction_id = fid;
                                sync_player_from_sides();
                              });
    ly = after_s1 + 18.f;
    float after_s2 =
        draw_side_army_picker(lx, ly, lw, list_h, deploy_side2_scroll, team2_faction_id, "SIDE 2 ARMY",
                              side2_home.c_str(), [&](int fid) {
                                if (fid == team1_faction_id) return;
                                team2_faction_id = fid;
                                sync_player_from_sides();
                              });

    ly = after_s2 + 20.f;
    section_label(lx, ly, "FIGHT FOR");
    ly += 22.f;
    const float pbw = (lw - 8.f) * 0.5f;
    const float pbh = 48.f;
    const auto& fac_s1 = dalian::faction_at(team1_faction_id);
    const auto& fac_s2 = dalian::faction_at(team2_faction_id);
    const bool play_side1 = player_team == dalian::TeamId::Team1;
    const bool play_side2 = player_team == dalian::TeamId::Team2;
    const bool hov_s1 = rect_hit(mx, my, lx, ly, pbw, pbh);
    const bool hov_s2 = rect_hit(mx, my, lx + pbw + 8.f, ly, pbw, pbh);
    renderer.ui_rect(lx, ly, pbw, pbh, play_side1 ? 0.12f : (hov_s1 ? 0.09f : 0.05f),
                     play_side1 ? 0.30f : (hov_s1 ? 0.14f : 0.08f),
                     play_side1 ? 0.48f : (hov_s1 ? 0.20f : 0.11f), 0.98f);
    renderer.ui_rect(lx + pbw + 8.f, ly, pbw, pbh, play_side2 ? 0.28f : (hov_s2 ? 0.14f : 0.05f),
                     play_side2 ? 0.14f : (hov_s2 ? 0.09f : 0.08f),
                     play_side2 ? 0.12f : (hov_s2 ? 0.10f : 0.11f), 0.98f);
    renderer.ui_text(lx + 8.f, ly + 8.f, 1.0f, "SIDE 1", 0.7f, 0.82f, 0.95f, 1.f);
    dalian::draw_clipped_text(renderer, lx + 8.f, ly + 26.f, pbw - 14.f, 1.05f, fac_s1.country, 0.9f,
                              0.92f, 0.94f, 1.f);
    renderer.ui_text(lx + pbw + 16.f, ly + 8.f, 1.0f, "SIDE 2", 0.95f, 0.65f, 0.55f, 1.f);
    dalian::draw_clipped_text(renderer, lx + pbw + 16.f, ly + 26.f, pbw - 14.f, 1.05f, fac_s2.country,
                              0.9f, 0.92f, 0.94f, 1.f);
    if (clicked && hov_s1) {
      player_team = dalian::TeamId::Team1;
      sync_player_from_sides();
      if (net.active()) net.set_faction(static_cast<std::uint16_t>(player_faction_id));
    }
    if (clicked && hov_s2) {
      player_team = dalian::TeamId::Team2;
      sync_player_from_sides();
      if (net.active()) net.set_faction(static_cast<std::uint16_t>(player_faction_id));
    }

    // ---- Middle: kit + loadout ----
    panel(mid_x, content_y, mid_w, content_h);
    const float kx = mid_x + 12.f;
    const float kw = mid_w - 24.f;
    float ky = content_y + 14.f;
    section_label(kx, ky, "SELECT KIT");
    ky += 26.f;
    const float kh = 36.f, kgap = 6.f;
    for (std::size_t i = 0; i < kits.size(); ++i) {
      const float y = ky + static_cast<float>(i) * (kh + kgap);
      const bool sel = static_cast<int>(i) == selected_kit;
      const bool hov = rect_hit(mx, my, kx, y, kw, kh);
      renderer.ui_rect(kx, y, kw, kh, sel ? 0.14f : (hov ? 0.11f : 0.06f),
                       sel ? 0.26f : (hov ? 0.14f : 0.08f), sel ? 0.42f : (hov ? 0.18f : 0.10f),
                       0.98f);
      renderer.ui_rect(kx, y, 3.f, kh, sel ? 0.4f : 0.22f, sel ? 0.75f : 0.35f, 1.0f, 1.f);
      dalian::draw_clipped_text(renderer, kx + 14.f, y + 10.f, kw - 22.f, 1.45f, kits[i].name, 0.93f,
                                0.95f, 0.97f, 1.f);
      if (clicked && hov) selected_kit = static_cast<int>(i);
    }

    const KitDef& k = kits[selected_kit];
    const int wf = dalian::faction_kit_side(player_faction_id);
    const char* wn = weapon_defs[static_cast<std::size_t>(k.weapon[wf]) % weapon_defs.size()].name;
    float sy = ky + static_cast<float>(kits.size()) * (kh + kgap) + 20.f;
    renderer.ui_rect(kx, sy - 8.f, kw, 1.f, 0.2f, 0.25f, 0.3f, 0.8f);
    section_label(kx, sy, "LOADOUT");
    char line[128];
    std::snprintf(line, sizeof(line), "Primary   %s", wn);
    dalian::draw_clipped_text(renderer, kx, sy + 26.f, kw, 1.25f, line, 0.86f, 0.88f, 0.9f, 1.f);
    std::snprintf(line, sizeof(line), "Gadget    %s", k.gadget);
    dalian::draw_clipped_text(renderer, kx, sy + 48.f, kw, 1.25f, line, 0.86f, 0.88f, 0.9f, 1.f);
    dalian::draw_clipped_text(renderer, kx, sy + 70.f, kw, 1.25f, "Sidearm   M9 Pistol", 0.7f, 0.72f,
                              0.75f, 1.f);

    // ---- Right: spawn map ----
    panel(map_x, content_y, map_w, map_h);
    const float mpx = map_x + 12.f;
    const float mpy = content_y + 14.f;
    const float mpw = map_w - 24.f;
    section_label(mpx, mpy, "SELECT SPAWN POINT");
    dalian::draw_clipped_text(renderer, mpx + 200.f, mpy + 2.f, mpw - 200.f, 1.0f,
                              "Your side's flags only", 0.5f, 0.7f, 0.9f, 0.9f);
    const float map_inner_y = mpy + 28.f;
    const float map_inner_h = map_h - 44.f;
    renderer.ui_rect(mpx, map_inner_y, mpw, map_inner_h, 0.05f, 0.08f, 0.07f, 1.f);
    renderer.ui_rect(mpx, map_inner_y, mpw, 2.f, 0.3f, 0.5f, 0.6f, 0.9f);
    renderer.ui_rect(mpx, map_inner_y + map_inner_h - 2.f, mpw, 2.f, 0.3f, 0.5f, 0.6f, 0.9f);

    glm::vec2 mn(1e30f, 1e30f), mx2(-1e30f, -1e30f);
    for (const auto& m : deploy_markers) {
      mn.x = std::min(mn.x, m.map_pos.x);
      mn.y = std::min(mn.y, m.map_pos.z);
      mx2.x = std::max(mx2.x, m.map_pos.x);
      mx2.y = std::max(mx2.y, m.map_pos.z);
    }
    glm::vec2 span = mx2 - mn;
    if (span.x < 1.f) span.x = 1.f;
    if (span.y < 1.f) span.y = 1.f;
    const float pad = 56.f;
    auto to_map = [&](const glm::vec3& p) {
      const float u = (p.x - mn.x) / span.x, v = (p.z - mn.y) / span.y;
      return glm::vec2(mpx + pad + u * (mpw - 2 * pad),
                       map_inner_y + pad + v * (map_inner_h - 2 * pad));
    };
    for (std::size_t i = 0; i < deploy_markers.size(); ++i) {
      const glm::vec2 c = to_map(deploy_markers[i].map_pos);
      const bool owned = spawn_point_owned(i);
      const bool sel = static_cast<int>(i) == selected_spawn;
      const float ms = sel ? 18.f : 14.f;
      const bool hov = owned && rect_hit(mx, my, c.x - ms * 0.5f, c.y - ms * 0.5f, ms, ms);
      float rr, rg, rb;
      if (owned) {
        rr = sel ? 0.2f : (hov ? 0.35f : 0.16f);
        rg = sel ? 0.75f : (hov ? 0.65f : 0.5f);
        rb = sel ? 1.0f : (hov ? 0.8f : 0.65f);
      } else {
        rr = 0.45f;
        rg = 0.18f;
        rb = 0.16f;
      }
      renderer.ui_rect(c.x - ms * 0.5f, c.y - ms * 0.5f, ms, ms, rr, rg, rb, owned ? 1.f : 0.5f);
      // Label below the marker so it never sits on the header / other columns.
      const float label_max = 130.f;
      const float label_x = std::clamp(c.x - label_max * 0.5f, mpx + 4.f, mpx + mpw - label_max - 4.f);
      const float label_y = std::min(c.y + ms * 0.5f + 4.f, map_inner_y + map_inner_h - 28.f);
      dalian::draw_clipped_text(renderer, label_x, label_y, label_max, 1.05f,
                                deploy_markers[i].name.c_str(), owned ? 0.88f : 0.55f,
                                owned ? 0.9f : 0.45f, owned ? 0.94f : 0.42f, owned ? 1.f : 0.7f);
      if (!owned) {
        const float lw = renderer.ui_text_width("LOCKED", 0.8f);
        renderer.ui_text(c.x - lw * 0.5f, label_y + 14.f, 0.8f, "LOCKED", 0.95f, 0.4f, 0.35f, 0.85f);
      }
      if (clicked && hov) selected_spawn = static_cast<int>(i);
    }

    const bool can_deploy =
        deploy_markers.empty() || spawn_point_owned(static_cast<std::size_t>(selected_spawn));

    // Deploy button — under the map panel, right-aligned.
    const float bw = 220.f, bh = btn_h;
    const float bx = map_x + map_w - bw;
    const float by = content_y + map_h + 10.f;
    const bool bhov = can_deploy && rect_hit(mx, my, bx, by, bw, bh);
    renderer.ui_rect(bx, by, bw, bh, bhov ? 0.14f : 0.09f, bhov ? 0.52f : 0.36f,
                     bhov ? 0.22f : 0.14f, can_deploy ? 1.f : 0.4f);
    const float tw = renderer.ui_text_width("DEPLOY", 2.4f);
    renderer.ui_text(bx + (bw - tw) * 0.5f, by + 14.f, 2.4f, "DEPLOY", can_deploy ? 0.96f : 0.5f,
                     can_deploy ? 1.0f : 0.5f, can_deploy ? 0.96f : 0.48f, 1.f);
    if (clicked && bhov) {
      apply_loadout();
      if (!match_started) game_sim.begin_match();
      if (have_game_audio) game_audio.play_weapon_deploy(resources, !third_person);
      deploy_open = false;
    }
  };

  bool jet_afterburner = false;
  std::uint32_t last_jet_w_tap_ms = 0;
  bool jet_w_was_down = false;
  float jet_w_hold_sec = 0.f;
  bool jet_w_hold_armed = false;  // one-shot per W press (prevents AB HUD flicker)
  float jet_ab_rearm_cd = 0.f;    // block re-latch after fuel empty
  bool jet_ab_lit = false;        // HUD/FX: physical AB (latch/Shift/Ctrl + fuel)
  bool jet_gear_toggle = false;
  float match_restart_cd = 0.f;
  dalian::HudFeed hud_feed;
  std::unordered_map<std::uint32_t, std::string> tracked_net_players;

  while (running) {
    bool launch_requested = false;  // AT rocket (RMB) or car SAM (map click)
    bool sam_launch_requested = false;
    bool kamikaze_launch_requested = false;
    bool heli_flare_requested = false;  // set by X while piloting a helicopter
    bool pending_enter_exit = false;
    int pending_seat_switch = -1;
    deploy_click = false;
    bool sam_map_click = false;
    air_stick_moved = false;
    jet_gear_toggle = false;
    jet_ab_lit = false;
    input.begin_frame();
    // Capture the cursor in-game; free it for deploy/pause menus.
    const bool mouse_look = !deploy_open && !pause_open && !sam_map_open;
    const SDL_bool want_rel = mouse_look ? SDL_TRUE : SDL_FALSE;
    if (SDL_GetRelativeMouseMode() != want_rel) SDL_SetRelativeMouseMode(want_rel);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        app_running = false;
        running = false;
      } else if (dalian::handle_display_hotkey(window, settings, e)) {
        dalian::refresh_display(window, renderer, cur_w, cur_h);
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        if (sam_map_open) {
          sam_map_open = false;
        } else if (deploy_open) {
          deploy_open = false;
        } else if (headless_shot) {
          running = false;
        } else {
          pause_open = true;
        }
      } else if (deploy_open && e.type == SDL_MOUSEWHEEL) {
        deploy_side1_scroll -= e.wheel.y * 24.f;
        deploy_side2_scroll -= e.wheel.y * 24.f;
      } else if (!pause_open && !deploy_open && e.type == SDL_MOUSEWHEEL &&
                 in_vehicle >= 0 && player_seat == 0 &&
                 in_vehicle < static_cast<int>(vehicles.size()) && vehicles[in_vehicle].is_air) {
        // c_PIThrottle analog — buffer wheel notches for the fixed physics tick.
        air_throttle_wheel_accum += static_cast<float>(e.wheel.y);
        input.handle_event(e, settings.bindings, false);
      } else if (deploy_open && e.type == SDL_MOUSEBUTTONDOWN &&
                 e.button.button == SDL_BUTTON_LEFT) {
        deploy_click = true;
      } else if (sam_map_open && e.type == SDL_MOUSEBUTTONDOWN &&
                 e.button.button == SDL_BUTTON_LEFT) {
        sam_map_click = true;
      } else if (!pause_open && !deploy_open &&
                 (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_MOUSEBUTTONDOWN ||
                  e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEWHEEL)) {
        input.handle_event(e, settings.bindings, false);
      } else if (e.type == SDL_MOUSEMOTION && mouse_look && drone_mode && !deploy_open) {
        drone_stick_roll += e.motion.xrel * 0.020f;
        drone_stick_pitch += e.motion.yrel * 0.020f;
      } else if (e.type == SDL_MOUSEMOTION && mouse_look && !deploy_open && !pause_open &&
                 air_input_grace <= 0.f && in_vehicle >= 0 && vehicles[in_vehicle].is_air &&
                 player_seat == 0) {
        // Buffer deltas for the fixed physics tick (virtual joystick accumulator).
        air_stick_moved = true;
        air_mouse_dx_accum += static_cast<float>(e.motion.xrel);
        air_mouse_dy_accum += static_cast<float>(e.motion.yrel);
      } else if (e.type == SDL_MOUSEMOTION && mouse_look && !deploy_open && !pause_open) {
        const float inv_y = settings.invert_mouse_y ? -1.f : 1.f;
        yaw += e.motion.xrel * sensitivity;
        pitch -= e.motion.yrel * sensitivity * inv_y;
        pitch = std::clamp(pitch, -89.f, 89.f);
      } else if (e.type == SDL_WINDOWEVENT &&
                 (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                  e.window.event == SDL_WINDOWEVENT_RESIZED ||
                  e.window.event == SDL_WINDOWEVENT_RESTORED ||
                  e.window.event == SDL_WINDOWEVENT_MAXIMIZED)) {
        dalian::refresh_display(window, renderer, cur_w, cur_h);
      }
    }

    if (!pause_open && !deploy_open) {
      const Uint8* kb = SDL_GetKeyboardState(nullptr);
      input.poll_keyboard(kb, settings.bindings);
      scoreboard_open = input.down(dalian::GameAction::Scoreboard);

      if (!drone_mode) {
        if (input.consume(dalian::GameAction::DeployScreen) && in_vehicle < 0 && !sam_map_open) {
          deploy_open = !deploy_open;
        }
        if (input.consume(dalian::GameAction::CycleCamera) && have_soldier) {
          third_person = !third_person;
        }
        if (input.consume(dalian::GameAction::EnterExit)) pending_enter_exit = true;
        for (int s = 0; s < 6; ++s) {
          if (input.consume(static_cast<dalian::GameAction>(
                  static_cast<int>(dalian::GameAction::Seat1) + s))) {
            pending_seat_switch = s;
          }
        }
        if (input.consume(dalian::GameAction::Reload) && in_vehicle < 0) {
          if (!reloading && reserve_ammo > 0 && mag_ammo < kMagSize) {
            reloading = true;
            reload_timer = 2.0f;
            if (have_game_audio) {
              game_audio.play_weapon_reload(resources, !third_person);
              game_audio.play_voice(resources, voice_bank, "AUTO_MOODGP_reloading");
            }
          }
        }
        if (in_vehicle >= 0 && in_vehicle < static_cast<int>(vehicles.size()) &&
            vehicles[in_vehicle].is_air && !vehicles[in_vehicle].is_heli && player_seat == 0 &&
            input.consume(dalian::GameAction::PickupKit)) {
          jet_gear_toggle = true;
        }
        if (in_vehicle < 0 && input.consume(dalian::GameAction::WeaponSlot4)) {
          if (grenades_left > 0) {
            const glm::vec3 eye0(player.position.x, player.position.y, player.position.z);
            glm::vec3 fr = bf2_forward(yaw, pitch);
            live_grenades.push_back({eye0 + fr * 0.6f, fr * 22.f + glm::vec3(0, 3.f, 0), 3.0f});
            --grenades_left;
            if (have_game_audio)
              game_audio.play_voice(resources, voice_bank, "AUTO_MOODGP_throwingfraggrenade");
          }
        }
        if (in_vehicle < 0 && input.consume(dalian::GameAction::WeaponSlot5) && c4_left > 0) {
          const glm::vec3 eye0(player.position.x, player.position.y, player.position.z);
          glm::vec3 fr = bf2_forward(yaw, pitch);
          const auto h = world.raycast({eye0.x, eye0.y, eye0.z}, {fr.x, fr.y, fr.z}, 6.f);
          const glm::vec3 pt =
              h.hit ? glm::vec3(h.point.x, h.point.y, h.point.z) : eye0 + fr * 3.f;
          placed_c4.push_back(pt);
          --c4_left;
        }
        if (input.consume(dalian::GameAction::SmokeFlares)) {
          if (in_vehicle >= 0 && vehicles[in_vehicle].is_air) {
            heli_flare_requested = true;
          } else if (in_vehicle < 0) {
            for (const auto& pt : placed_c4) explode_at(pt, 7.f, 160.f);
            placed_c4.clear();
          }
        }
        if (in_vehicle < 0 && input.consume(dalian::GameAction::MedkitHeal)) {
          if (has_medkit && medkit_cd <= 0.f && player_health < 100.f) {
            player_health = std::min(100.f, player_health + 45.f);
            medkit_cd = 6.f;
          }
        }
        if (input.consume(dalian::GameAction::KamikazeDrone)) kamikaze_launch_requested = true;
        if (input.consume(dalian::GameAction::CarSam)) {
          // Toggle tactical map when near a launch vehicle or Hawk emplacement.
          if (sam_map_open) {
            sam_map_open = false;
            sam_launch_mode = 0;
          } else if (in_vehicle < 0) {
            bool near_car = false;
            for (const auto& v : vehicles) {
              if (v.is_air || v.mesh_key.find("vehicles/") == std::string::npos) continue;
              const float dx = v.pos.x - player.position.x;
              const float dz = v.pos.z - player.position.z;
              if (dx * dx + dz * dz < 28.f * 28.f) {
                near_car = true;
                break;
              }
            }
            const bool near_hawk =
                hawk_present && hawk_ammo > 0 &&
                [&]() {
                  const float dx = hawk_pos.x - player.position.x;
                  const float dz = hawk_pos.z - player.position.z;
                  return dx * dx + dz * dz < 14.f * 14.f;
                }();
            if (near_hawk) {
              sam_launch_mode = 1;
              sam_map_open = true;
            } else if (near_car) {
              sam_launch_mode = 0;
              sam_map_open = true;
            }
          }
        }
        // E on Hawk opens the same map UI (does not enter a vehicle).
        if (pending_enter_exit && in_vehicle < 0 && hawk_present && hawk_ammo > 0) {
          const float dx = hawk_pos.x - player.position.x;
          const float dz = hawk_pos.z - player.position.z;
          if (dx * dx + dz * dz < 14.f * 14.f) {
            // Only steal E if no closer enterable vehicle.
            bool closer_veh = false;
            for (const auto& v : vehicles) {
              if (v.mesh_key.find("vehicles/") == std::string::npos) continue;
              const float vx = v.pos.x - player.position.x;
              const float vz = v.pos.z - player.position.z;
              if (vx * vx + vz * vz < 6.f * 6.f) {
                closer_veh = true;
                break;
              }
            }
            if (!closer_veh) {
              sam_launch_mode = 1;
              sam_map_open = true;
              pending_enter_exit = false;
            }
          }
        }
        if (input.consume(dalian::GameAction::ReconDrone)) {
          if (!kamikaze_mode) {
            drone_mode = !drone_mode;
            if (drone_mode) {
              drone = bf2::DroneController{};
              const glm::vec3 launch(player.position.x, player.position.y + 0.6f,
                                     player.position.z);
              drone.position = launch;
              drone_prev_pos = launch;
              drone_throttle = 0.30f;
              signal = 1.f;
            }
          }
        }
      }

      const int wheel = input.weapon_wheel_delta();
      if (!drone_mode && !deploy_open && wheel != 0) {
        const int dir = wheel > 0 ? 1 : -1;
        for (std::size_t step = 0; step < weapon_defs.size(); ++step) {
          weapon_index = (weapon_index + weapon_defs.size() + dir) % weapon_defs.size();
          if (load_weapon(weapon_index)) {
            bind_weapon_audio();
            bind_weapon_sim(true);
            break;
          }
        }
        bind_weapon_audio();
        input.clear_wheel_delta();
      }
    }

    if (pause_open && !headless_shot) {
      bool opts_flag = false;
      const dalian::PauseResult pr =
          dalian::run_pause_overlay(window, renderer, settings, cur_w, cur_h, &opts_flag);
      pause_open = false;
      if (pr.resume) {
        // continue playing
      } else if (pr.leave_to_menu) {
        leave_to_menu = true;
        running = false;
      } else if (pr.quit_app) {
        app_running = false;
        running = false;
      }
      sensitivity = settings.mouse_sensitivity;
      draw_dist = settings.draw_distance;
      draw_dist2 = draw_dist * draw_dist;
      prev = SDL_GetPerformanceCounter();
      continue;
    }

    const Uint64 now = SDL_GetPerformanceCounter();
    const float raw_dt =
        static_cast<float>(now - prev) / static_cast<float>(SDL_GetPerformanceFrequency());
    prev = now;
    float dt = std::clamp(raw_dt, 0.f, 0.05f);
    // Smoothed FPS from unclamped frame time (clamped dt would lie under hitches).
    static float fps_smooth = 60.f;
    if (raw_dt > 1e-6f) {
      const float inst = 1.f / raw_dt;
      fps_smooth = fps_smooth * 0.9f + inst * 0.1f;
    }
    if (!deploy_open && !drone_mode) anim_time += dt;

    // Pump the network early so this frame renders the freshest remote states.
    // `net_fired` / `net_fire_*` capture a shot taken this frame for replication.
    if (net.active()) net.poll(dt);
    if (session_mp.enabled && !session_mp.is_host && net.connection_lost()) {
      dalian::run_connection_lost_dialog(window, renderer, cur_w, cur_h, "CONNECTION LOST",
                                         net.connection_lost_message());
      net.clear_connection_lost();
      leave_to_menu = true;
      running = false;
    }
    if (net.active()) {
      std::unordered_set<std::uint32_t> active_remotes;
      for (const auto& p : net.players()) {
        if (!p.active || p.id == net.local_id()) continue;
        active_remotes.insert(p.id);
        if (!tracked_net_players.count(p.id)) {
          std::string pname = net.lobby_player_name(p.id);
          if (pname.empty()) pname = "Player";
          tracked_net_players[p.id] = pname;
          char msg[128];
          std::snprintf(msg, sizeof(msg), "%s joined the battle", pname.c_str());
          hud_feed.push(msg, 8.f, dalian::HudFeedLine::Kind::Join);
        }
      }
      for (auto it = tracked_net_players.begin(); it != tracked_net_players.end();) {
        if (active_remotes.count(it->first)) {
          ++it;
          continue;
        }
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s left the server", it->second.c_str());
        hud_feed.push(msg, 8.f, dalian::HudFeedLine::Kind::Leave);
        it = tracked_net_players.erase(it);
      }
    }
    hud_feed.tick(dt);
    if (advertise_session) {
      dalian::DiscoveryAdvert adv{};
      adv.game_port = session_mp.enabled ? session_mp.port : net_port;
      adv.host_name =
          session_mp.player_name.empty() ? settings.player_name : session_mp.player_name;
      adv.allow_late_join = session_mp.enabled ? session_mp.allow_late_join : true;
      adv.in_game = true;
      int pc = 0;
      for (const auto& p : net.players())
        if (p.active) ++pc;
      adv.players = pc;
      adv.map_name = map_label;
      session_discovery.set_advert(adv);
      session_discovery.poll();
    }
    bool net_fired = false;
    glm::vec3 net_fire_o(0.f), net_fire_d(0.f);

    // Camera basis — BF2 yaw 0 = north (+Z). OpenGL RH lookAt mirrors L/R vs BF2's
    // left-handed world, so we flip projection X and use the LH strafe axis.
    glm::vec3 front = bf2_forward(yaw, pitch);
    glm::vec3 flat_front(0.f, 0.f, 1.f);
    if (const float flat_len2 = front.x * front.x + front.z * front.z; flat_len2 > 1e-6f) {
      flat_front = glm::normalize(glm::vec3(front.x, 0.f, front.z));
    }
    const glm::vec3 right = bf2_right(flat_front);

    if (kamikaze_launch_requested) {
      drone_mode = true;
      kamikaze_mode = true;
      const auto k = bf2::KamikazeDrone::spawn();
      drone = k.body;
      drone.position =
          glm::vec3(player.position.x, player.position.y + 1.1f, player.position.z) +
          flat_front * 0.35f;
      drone_prev_pos = drone.position;
      drone_throttle = 0.55f;
      drone_stick_pitch = 0.f;
      drone_stick_roll = 0.f;
      signal = 1.f;
    }

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const Uint32 mouse = SDL_GetMouseState(nullptr, nullptr);
    voice_cooldown = std::max(0.f, voice_cooldown - dt);
    medkit_cd = std::max(0.f, medkit_cd - dt);
    sam_hud_flash = std::max(0.f, sam_hud_flash - dt);

    // Resolve SAM map click before the sim tick so the missile launches this frame.
    if (sam_map_open && sam_map_click) {
      int lmx = 0, lmy = 0;
      SDL_GetMouseState(&lmx, &lmy);
      float dx = 0.f, dy = 0.f;
      dalian::ui_mouse_design(renderer, lmx, lmy, dx, dy);
      const float map_x = 80.f, map_y = 100.f;
      const float map_w = 1600.f - 160.f, map_h = 900.f - 180.f;
      glm::vec2 mn(1e30f), mxb(-1e30f);
      auto expand = [&](float x, float z) {
        mn.x = std::min(mn.x, x);
        mn.y = std::min(mn.y, z);
        mxb.x = std::max(mxb.x, x);
        mxb.y = std::max(mxb.y, z);
      };
      expand(player.position.x, player.position.z);
      for (const auto& cp : conquest_points) expand(cp.pos.x, cp.pos.z);
      for (const auto& v : vehicles) {
        if (v.mesh_key.find("vehicles/") != std::string::npos) expand(v.pos.x, v.pos.z);
      }
      for (const auto& m : deploy_markers) expand(m.map_pos.x, m.map_pos.z);
      if (mxb.x - mn.x < 120.f) {
        const float c = 0.5f * (mn.x + mxb.x);
        mn.x = c - 60.f;
        mxb.x = c + 60.f;
      }
      if (mxb.y - mn.y < 120.f) {
        const float c = 0.5f * (mn.y + mxb.y);
        mn.y = c - 60.f;
        mxb.y = c + 60.f;
      }
      const float pad = 28.f;
      dalian::MinimapProjector sam_map;
      sam_map.configure(mn, mxb, map_x + pad, map_y + pad, map_w - 2.f * pad, map_h - 2.f * pad);
      if (sam_map.contains_screen(dx, dy)) {
        if (sam_launch_mode == 1 && hawk_present && hawk_ammo <= 0) {
          sam_map_open = false;
        } else {
          sam_pending_target = sam_map.minimap_to_world(dx, dy);
          sam_pending_target.y =
              world.terrain_height(sam_pending_target.x, sam_pending_target.z) + 1.5f;
          sam_has_pending_target = true;
          sam_launch_requested = true;
          sam_hud_flash = 2.0f;
          if (sam_launch_mode == 1 && hawk_present) {
            const float dxw = sam_pending_target.x - hawk_pos.x;
            const float dzw = sam_pending_target.z - hawk_pos.z;
            hawk_heading = glm::degrees(std::atan2(dxw, dzw));
            hawk_aim_pitch = 35.f;
            --hawk_ammo;
          }
          sam_map_open = false;
        }
      }
    }
    snapshot_log_timer += dt;
    if (match_over) match_restart_cd += dt;
    if (match_over && match_started && match_restart_cd >= 12.f) {
      game_sim.restart_round();
      deploy_open = true;
      match_restart_cd = 0.f;
      player_health = 100.f;
      player_stamina = 100.f;
    }

    if (!drone_mode) {
      if (shot_missile && frame_no == std::max(2, shot_frames - 12)) launch_requested = true;
      dalian::PlayerInput inp{};
      const bool in_air_pilot =
          in_vehicle >= 0 && in_vehicle < static_cast<int>(vehicles.size()) &&
          vehicles[in_vehicle].is_air && player_seat == 0;
      const bool in_jet_pilot = in_air_pilot && !vehicles[in_vehicle].is_heli;
      if (!deploy_open && !sam_map_open) {
        glm::vec3 move(0.f);
        if (input.down(dalian::GameAction::MoveForward)) move += flat_front;
        if (input.down(dalian::GameAction::MoveBack)) move -= flat_front;
        if (input.down(dalian::GameAction::StrafeRight)) move += right;
        if (input.down(dalian::GameAction::StrafeLeft)) move -= right;
        inp.move_wish = move;
        if (in_air_pilot) {
          inp.boost = input.down(dalian::GameAction::Sprint);
          inp.pitch_up = input.down(dalian::GameAction::Jump);
          inp.jump = false;
        } else {
          inp.sprint = input.down(dalian::GameAction::Sprint);
          inp.jump = input.down(dalian::GameAction::Jump);
          inp.crouch = input.down(dalian::GameAction::Crouch);
          if (input.consume(dalian::GameAction::Prone)) inp.prone_toggle = true;
        }
        const bool w_down = input.down(dalian::GameAction::MoveForward);
        if (in_jet_pilot) {
          if (jet_ab_rearm_cd > 0.f) jet_ab_rearm_cd = std::max(0.f, jet_ab_rearm_cd - dt);
          // Wall-clock double-tap (round_time resets on match restart and breaks the window).
          if (w_down && !jet_w_was_down) {
            const std::uint32_t tap_now = SDL_GetTicks();
            if (jet_ab_rearm_cd <= 0.f && last_jet_w_tap_ms != 0 &&
                (tap_now - last_jet_w_tap_ms) < 380u) {
              jet_afterburner = !jet_afterburner;
            }
            last_jet_w_tap_ms = tap_now;
            jet_w_hold_armed = false;
          }
          if (w_down)
            jet_w_hold_sec += dt;
          else {
            jet_w_hold_sec = 0.f;
            jet_w_hold_armed = false;
          }
          // Hold W ~0.45s while spooled → arm AB once per press (not every frame).
          const float sprint =
              in_vehicle >= 0 ? vehicles[in_vehicle].jet_sprint : 0.f;
          if (!jet_w_hold_armed && w_down && jet_w_hold_sec >= 0.45f && jet_ab_rearm_cd <= 0.f &&
              in_vehicle >= 0 && vehicles[in_vehicle].throttle >= 0.85f && sprint > 0.12f) {
            jet_afterburner = true;
            jet_w_hold_armed = true;
          }
          // Ctrl / Shift = hold-to-AB; double-tap / long-hold W latch until fuel empty or toggle off.
          const bool ctrl_ab = input.down(dalian::GameAction::Crouch);
          if (jet_afterburner && sprint <= 0.01f) {
            jet_afterburner = false;
            jet_ab_rearm_cd = 1.25f;  // stop hold-W / fuel edge from strobing HUD
          }
          const bool want_ab =
              inp.boost || ctrl_ab || (jet_afterburner && sprint > 0.01f);
          inp.boost = want_ab;
          jet_ab_lit = want_ab && sprint > 0.01f;
        } else {
          jet_w_hold_sec = 0.f;
          jet_w_hold_armed = false;
          jet_ab_lit = false;
        }
        jet_w_was_down = w_down;
        inp.gear_toggle = jet_gear_toggle;
        inp.throttle_up = input.down(dalian::GameAction::MoveForward);
        inp.throttle_down = input.down(dalian::GameAction::MoveBack);
        inp.yaw_left = input.down(dalian::GameAction::StrafeLeft);
        inp.yaw_right = input.down(dalian::GameAction::StrafeRight);
      }
      const bool rmb = (mouse & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
      if (!deploy_open && in_vehicle < 0 && has_at && rmb && !rmb_was_down) {
        launch_requested = true;
      }
      rmb_was_down = rmb;
      inp.look_forward = front;
      inp.look_right = right;
      inp.eye = glm::vec3(player.position.x, player.position.y, player.position.z);
      inp.fire = (mouse & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
      inp.fire_secondary = (mouse & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
      inp.invert_air = settings.invert_air;
      inp.deploy_open = deploy_open;
      inp.mouse_look = mouse_look;
      inp.drone_mode = false;
      inp.reloading_blocked = reloading;
      inp.air_pitch_stick = air_pitch_stick;
      inp.air_roll_stick = air_roll_stick;
      inp.air_yaw_stick = G.air_yaw_stick;
      inp.air_mouse_dx = air_mouse_dx_accum;
      inp.air_mouse_dy = air_mouse_dy_accum;
      inp.air_mouse_sens = glm::clamp(sensitivity / 0.18f, 0.4f, 1.35f);
      inp.air_throttle_delta = air_throttle_wheel_accum;
      inp.air_stick_moved = air_stick_moved;
      air_mouse_dx_accum = 0.f;
      air_mouse_dy_accum = 0.f;
      air_throttle_wheel_accum = 0.f;
      inp.enter_exit = pending_enter_exit;
      inp.seat_switch = pending_seat_switch;
      inp.launch_at = launch_requested && has_at;
      // Car SAM / Hawk: map click sets destination.
      inp.launch_missile = sam_launch_requested && in_vehicle < 0 && sam_has_pending_target;
      inp.has_missile_target = sam_has_pending_target;
      inp.missile_target = sam_pending_target;
      if (sam_launch_requested && sam_launch_mode == 1 && hawk_present) {
        inp.has_sam_launch_origin = true;
        // Launch from above the rail / tower.
        inp.sam_launch_origin = hawk_pos + glm::vec3(0.f, 5.5f, 0.f);
      } else {
        inp.has_sam_launch_origin = false;
      }
      inp.flare_request = heli_flare_requested;
      pending_enter_exit = false;
      pending_seat_switch = -1;
      launch_requested = false;
      sam_launch_requested = false;
      sam_has_pending_target = false;
      heli_flare_requested = false;
      last_net_inp = inp;
      int humans = 1;
      if (net.active() && !deploy_open) humans = 1 + net.peer_count();
      game_sim.set_connected_humans(humans);
      game_sim.tick(dt, inp);
      for (const auto& kf : game_sim.events().kill_feed) {
        hud_feed.push_kill(kf.killer, kf.victim, kf.weapon);
      }
      if (!ambient_emitters.empty()) {
        dalian::step_ambient_emitters(ambient_emitters, smoke, dt);
      }
      if (in_jet_pilot && jet_ab_lit && in_vehicle >= 0) {
        const Vehicle& jv = vehicles[in_vehicle];
        for (const glm::vec3& ex : {jv.jet_exhaust_l, jv.jet_exhaust_r}) {
          const glm::vec4 wp = jv.model * glm::vec4(ex, 1.f);
          const glm::vec3 back = glm::normalize(glm::vec3(jv.model[2]));
          spawn_jet_exhaust_fx(smoke, glm::vec3(wp), -back, true, 2);
        }
      }
      const auto& ev = game_sim.events();
      if (ev.fired_shot) {
        net_fired = true;
        net_fire_o = ev.fire_origin;
        net_fire_d = ev.fire_dir;
        if (have_game_audio) game_audio.play_weapon_fire(resources, !third_person);
      }
      if (ev.play_reload && have_game_audio) {
        game_audio.play_weapon_reload(resources, !third_person);
        game_audio.play_voice(resources, voice_bank, "AUTO_MOODGP_reloading");
      }
      if (ev.play_artillery_explosion && have_game_audio) {
        const auto& p = ev.artillery_explosion_pos;
        static const char* kBoomPaths[] = {
            "Objects/Vehicles/Common/Sounds/exp_tank.wav",
            "Objects/Weapons/Common/Sounds/exp_large.wav",
            "Objects/Weapons/Common/Sounds/exp_medium.wav",
            "Objects/Effects/Sounds/exp_generic.wav",
        };
        bool played = false;
        for (const char* path : kBoomPaths) {
          if (resources.read_bytes(path)) {
            game_audio.play_3d(resources, path, p.x, p.y, p.z, player.position.x, player.position.y,
                               player.position.z, 1.55f);
            game_audio.play_2d(resources, path, 0.85f);
            played = true;
            break;
          }
        }
        if (!played) {
          for (const char* path : kBoomPaths) game_audio.play_2d(resources, path, 1.2f);
        }
      }
      if (ev.out_of_ammo_voice && voice_cooldown <= 0.f && have_game_audio) {
        game_audio.play_voice(resources, voice_bank, "out_of_ammo");
        voice_cooldown = 2.5f;
      }
      if (ev.open_deploy && !match_over) {
        deploy_open = true;
        pick_spawnable();
      }
      if (!ev.voice_cue.empty() && have_game_audio && voice_cooldown <= 0.f) {
        if (dalian::play_conquest_voice(resources, game_audio, voice_bank, ev.voice_cue)) {
          voice_cooldown = 1.8f;
        }
      }
      if (ev.vehicle_exited) {
        yaw = ev.exit_yaw_deg;
        air_pitch_stick = air_roll_stick = 0.f;
        G.air_yaw_stick = 0.f;
        jet_afterburner = false;
        jet_ab_lit = false;
        jet_w_hold_sec = 0.f;
        jet_w_hold_armed = false;
        jet_ab_rearm_cd = 0.f;
        last_jet_w_tap_ms = 0;
        jet_w_was_down = false;
      }
      if (ev.capture_mouse && !deploy_open && !pause_open) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
      }
      if (ev.discard_mouse_delta) {
        int dx = 0, dy = 0;
        SDL_GetRelativeMouseState(&dx, &dy);
      }
      if (std::getenv("BF2_SNAPSHOT_DEBUG") && snapshot_log_timer >= 1.f) {
        snapshot_log_timer = 0.f;
        const auto snap = game_sim.build_snapshot(1, yaw);
        std::cout << "[snapshot] t=" << snap.round_time << " tickets=" << snap.tickets.team1_tickets
                  << "/" << snap.tickets.team2_tickets << " cps=" << snap.control_points.size()
                  << " vehicles=" << snap.vehicles.size() << " projectiles=" << snap.projectiles.size()
                  << '\n';
      }
    }

    // Vehicle engine / tire audio for the occupied vehicle.
    if (have_game_audio) {
      if (in_vehicle < 0 && veh_audio_idx >= 0) {
        const auto sit = vehicle_sounds.find(vehicles[veh_audio_idx].mesh_key);
        if (sit != vehicle_sounds.end() && !sit->second.engine_stop.empty())
          game_audio.play_2d(resources, sit->second.engine_stop, 0.85f);
        game_audio.stop_channel(veh_engine_ch);
        game_audio.stop_channel(veh_tire_ch);
        veh_engine_ch = veh_tire_ch = -1;
        veh_audio_idx = -1;
      } else if (in_vehicle >= 0 && in_vehicle < static_cast<int>(vehicles.size())) {
        Vehicle& av = vehicles[in_vehicle];
        const auto sit = vehicle_sounds.find(av.mesh_key);
        if (sit != vehicle_sounds.end()) {
          const dalian::VehicleSoundSet& snd = sit->second;
          if (veh_audio_idx != in_vehicle) {
            game_audio.stop_channel(veh_engine_ch);
            game_audio.stop_channel(veh_tire_ch);
            if (!snd.engine_start.empty()) game_audio.play_2d(resources, snd.engine_start, 0.9f);
            veh_engine_ch =
                snd.engine_loop.empty() ? -1 : game_audio.play_loop(resources, snd.engine_loop, 0.4f);
            veh_tire_ch = -1;
            veh_audio_idx = in_vehicle;
          }
          const float eng_norm =
              av.is_air ? av.throttle : std::clamp(std::fabs(av.speed) / 22.f, 0.f, 1.f);
          game_audio.set_channel_volume(veh_engine_ch, 0.15f + eng_norm * 0.75f);
          const bool rolling = !av.is_air && std::fabs(av.speed) > 0.4f;
          if (rolling && !snd.tire_roll.empty()) {
            if (!game_audio.channel_playing(veh_tire_ch))
              veh_tire_ch = game_audio.play_loop(resources, snd.tire_roll, 0.25f);
            game_audio.set_channel_volume(veh_tire_ch,
                                           std::clamp(std::fabs(av.speed) / 18.f, 0.08f, 0.65f));
          } else {
            game_audio.stop_channel(veh_tire_ch);
            veh_tire_ch = -1;
          }
        }
      }
    }

    // Drone piloting (not yet in GameSim).
    if (drone_mode) {
      // Fly the quad: W/S set throttle, A/D yaw, mouse pitch/roll (self-centring).
      if (keys != nullptr) {
        if (keys[SDL_SCANCODE_W]) drone_throttle += 0.6f * dt;
        if (keys[SDL_SCANCODE_S]) drone_throttle -= 0.6f * dt;
        drone_throttle = std::clamp(drone_throttle, 0.f, 1.f);
      }
      bf2::DroneController::Input in;
      in.throttle = drone_throttle;
      in.pitch = std::clamp(drone_stick_pitch, -1.f, 1.f);
      in.roll = std::clamp(drone_stick_roll, -1.f, 1.f);
      if (keys != nullptr) {
        if (keys[SDL_SCANCODE_A]) in.yaw -= 1.f;
        if (keys[SDL_SCANCODE_D]) in.yaw += 1.f;
      }
      const float gh = world.terrain_height(drone.position.x, drone.position.z);
      drone.update(in, dt > 0.f ? dt : 1.f / 60.f, gh);
      // Sticks self-centre once the mouse stops moving.
      drone_stick_pitch *= std::max(0.f, 1.f - dt * 12.f);
      drone_stick_roll *= std::max(0.f, 1.f - dt * 12.f);

      // Kamikaze loitering munition: detonate on impact, manual trigger, or dead battery.
      if (kamikaze_mode) {
        const Uint32 kmouse = SDL_GetMouseState(nullptr, nullptr);
        const bool kamikaze_fire = (kmouse & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        const bool manual =
            kamikaze_fire || (keys != nullptr && keys[SDL_SCANCODE_SPACE]);
        glm::vec3 boom_pt = drone.position;
        bool boom = manual || drone.battery <= 0.f;
        if (!boom) {
          const glm::vec3 seg = drone.position - drone_prev_pos;
          const float seg_len = glm::length(seg);
          if (seg_len > 1e-4f) {
            const glm::vec3 sd = seg / seg_len;
            const auto hit = world.raycast({drone_prev_pos.x, drone_prev_pos.y, drone_prev_pos.z},
                                           {seg.x, seg.y, seg.z}, seg_len);
            const float terr_d = hit.hit ? hit.distance : 1e30f;
            const auto eh = shoot_enemies(drone_prev_pos, sd, seg_len);
            if (eh.idx >= 0 && eh.dist < terr_d) {
              boom = true;
              boom_pt = eh.point;
            } else if (hit.hit) {
              boom = true;
              boom_pt = glm::vec3(hit.point.x, hit.point.y, hit.point.z);
            }
          }
        }
        if (boom) {
          detonate_kamikaze(boom_pt);
        } else {
          drone_prev_pos = drone.position;
        }
      } else {
        drone_prev_pos = drone.position;
      }

      // The operator's body stays put while piloting.
      player.desired_velocity = {0.f, 0.f, 0.f};
      world.step_character(player, dt > 0.f ? dt : 1.f / 60.f);
    }

    const glm::vec3 eye(player.position.x, player.position.y, player.position.z);

    // Deterministic timestep for client-only gadgets in headless capture.
    const float mdt = shot_path.empty() ? dt : (1.f / 60.f);

    // Thrown frag grenades: ballistic arc, detonate on ground contact or fuse.
    for (auto& gr : live_grenades) {
      gr.vel.y -= 9.8f * mdt;
      const glm::vec3 prev = gr.pos;
      gr.pos += gr.vel * mdt;
      gr.fuse -= mdt;
      const float th = world.terrain_height(gr.pos.x, gr.pos.z);
      const bool ground = gr.pos.y <= th;
      if (ground || gr.fuse <= 0.f) {
        glm::vec3 boom = gr.pos;
        if (ground) boom.y = th;
        explode_at(boom, 8.f, 110.f);
        gr.fuse = -1.f;  // mark for removal
      }
      (void)prev;
    }
    live_grenades.erase(std::remove_if(live_grenades.begin(), live_grenades.end(),
                                       [](const Grenade& g) { return g.fuse <= -0.5f; }),
                        live_grenades.end());

    // ---- Multiplayer: publish our state, ingest remote shots ----------------
    if (net.active()) {
      bf2::NetPlayer me;
      me.id = net.local_id();
      me.active = true;
      const float send_dt =
          net_last_send_at >= 0.0 ? static_cast<float>(net.net_time() - net_last_send_at) : dt;
      const bf2::NetInput net_in =
          dalian::fill_net_input(last_net_inp, yaw, pitch, net_input_seq);
      dalian::fill_net_player(me, player, yaw, pitch, anim_time, game_sim.state().infantry_pose,
                              in_vehicle, vehicles, player_health,
                              static_cast<std::uint16_t>(player_faction_id), net_fired, send_dt,
                              net_in.seq, player_seat);
      bf2::NetShot sh;
      if (net_fired) {
        sh.shooter = net.local_id();
        sh.ox = net_fire_o.x; sh.oy = net_fire_o.y; sh.oz = net_fire_o.z;
        sh.dx = net_fire_d.x; sh.dy = net_fire_d.y; sh.dz = net_fire_d.z;
      }
      net.send_local(me, net_fired ? &sh : nullptr);
      net.send_input(net_in);
      net_last_send_at = net.net_time();
      net.update_remotes();
      dalian::apply_mp_vehicle_sync(vehicles, net, in_vehicle, game_sim);
      // Remote shots: draw the tracer + impact locally so gunfire is visible.
      for (const auto& s : net.take_shots()) {
        const glm::vec3 o(s.ox, s.oy, s.oz), d(s.dx, s.dy, s.dz);
        const auto hit = world.raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, 400.f);
        const glm::vec3 end =
            hit.hit ? glm::vec3(hit.point.x, hit.point.y, hit.point.z) : o + d * 400.f;
        tracers.push_back({o, end, 0.06f});
        if (hit.hit) impacts.push_back({end, 0.5f});
      }
    }

    // Camera: FPV from the drone when flying, else eye (1st) / behind (3rd).
    dalian::refresh_display(window, renderer, cur_w, cur_h);
    glm::vec3 cam = eye;
    glm::vec3 cam_front = front;
    glm::vec3 cam_up(0.f, 1.f, 0.f);
    float cam_fov = settings.fov;
    if (drone_mode) {
      cam = drone.position;
      cam_front = drone.forward(25.f);  // FPV cams sit tilted up
      cam_up = drone.up();              // bank with the airframe for immersion
      cam_fov = 95.f;                   // wide FPV lens
      // Link quality: falls with range from the operator and near the ground.
      // Kamikaze munitions use cheaper radios — signal degrades faster.
      const float range = glm::distance(cam, eye);
      const float max_link = kamikaze_mode ? 320.f : 600.f;
      const float rq = std::clamp(1.f - range / max_link, 0.f, 1.f);
      signal = std::clamp(signal + (rq - signal) * std::min(dt * 2.f, 1.f), 0.f, 1.f);
    } else if (in_vehicle >= 0) {
      const Vehicle& v = vehicles[in_vehicle];
      float dist = v.is_air ? 12.f : 8.5f;
      if (const char* cd = std::getenv("BF2_CHASEDIST")) dist = std::atof(cd);
      if (v.is_air && player_seat == 0) {
        const glm::vec3 center = v.pos + glm::vec3(0.f, 1.5f, 0.f);
        if (!v.is_heli && !v.jet_airborne) {
          // Runway chase: level view while taxiing / rolling (BF2 — mouse not flying yet).
          const float hd = glm::radians(v.heading);
          const glm::vec3 flat_fwd(std::sin(hd), 0.f, std::cos(hd));
          cam = center - flat_fwd * dist + glm::vec3(0.f, dist * 0.22f, 0.f);
          const float ground = world.terrain_height(cam.x, cam.z) + 0.8f;
          if (cam.y < ground) cam.y = ground;
          cam_front = glm::normalize((center + flat_fwd * 8.f) - cam);
          cam_up = glm::vec3(0.f, 1.f, 0.f);
        } else {
          // In-flight chase: follows heading + pitch + bank.
          const float hd = glm::radians(v.heading);
          const float pr = glm::radians(-v.pitch);
          const float rl = glm::radians(-v.roll);
          glm::mat4 orient = glm::rotate(glm::mat4(1.f), hd, glm::vec3(0, 1, 0));
          orient = glm::rotate(orient, pr, glm::vec3(1, 0, 0));
          orient = glm::rotate(orient, rl, glm::vec3(0, 0, 1));
          const glm::vec3 nose = glm::normalize(glm::vec3(orient * glm::vec4(0, 0, 1, 0)));
          const glm::vec3 up = glm::normalize(glm::vec3(orient * glm::vec4(0, 1, 0, 0)));
          cam = center + up * 0.2f - nose * dist + up * (dist * 0.22f);
          const float ground = world.terrain_height(cam.x, cam.z) + 0.8f;
          if (cam.y < ground) cam.y = ground;
          cam_front = glm::normalize((center + nose * 10.f) - cam);
          cam_up = up;
        }
        cam_fov = 74.f;
      } else {
        // Orbit chase cam (ground vehicles, or a gunner seat): mouse aims the view.
        const glm::vec3 center(v.pos.x, v.pos.y + 2.0f, v.pos.z);
        cam = center - front * dist;
        const float ground = world.terrain_height(cam.x, cam.z) + 0.6f;
        if (cam.y < ground) cam.y = ground;
        cam_front = glm::normalize(center - cam);
        cam_fov = 74.f;
      }
    } else if (third_person) {
      const float back = 3.0f;
      const float up = 0.7f;
      cam = eye - front * back + glm::vec3(0, 1, 0) * up;
      const float ground = world.terrain_height(cam.x, cam.z) + 0.3f;
      if (cam.y < ground) cam.y = ground;  // don't sink the camera into terrain
    }
    const float far_z = std::max(12000.f, draw_dist * 1.35f);
    const glm::mat4 proj_rh = glm::perspective(
        glm::radians(cam_fov), static_cast<float>(cur_w) / std::max(cur_h, 1), 0.2f, far_z);
    const glm::mat4 view = glm::lookAt(cam, cam + cam_front, cam_up);
    // BF2 world is left-handed; RH lookAt puts east on the left when facing north.
    // Mirror clip-space X so on-screen layout matches retail BF2 (swap winding).
    const glm::mat4 proj =
        glm::scale(glm::mat4(1.f), glm::vec3(-1.f, 1.f, 1.f)) * proj_rh;
    const glm::mat4 view_proj = proj * view;
    // Refit the shadow cascades to this frame's camera and sun (matrices ready
    // fit the cascades to this frame's camera + sun, then render depth for each.
    // Cover a longer shadow range now that far geometry is visible.
    // Use the unmirrored RH view — cascade math assumes camera looks down -Z.
    const float shadow_range =
        std::min(settings.shadow_distance, std::min(2200.f, draw_dist * 0.35f));
    csm.update(view, glm::radians(cam_fov), static_cast<float>(cur_w) / std::max(cur_h, 1), 0.2f,
               shadow_range, atmo.sun_dir);
    const glm::mat4 inv_view_proj = glm::inverse(view_proj);

    // ---- Shadow depth passes: buildings + vehicles cast into each cascade. ----
    const float shadow_cast_dist2 = shadow_range * shadow_range;
    float cascade_vp[bf2::Renderer::kShadowCascades * 16];
    float cascade_splits[4];
    if (settings.shadows_enabled) {
      for (int c = 0; c < bf2::Renderer::kShadowCascades; ++c) {
        const glm::mat4& vp = csm.cascade(c).view_proj;
        std::memcpy(&cascade_vp[c * 16], glm::value_ptr(vp), 16 * sizeof(float));
        cascade_splits[c] = csm.cascade(c).split_far;
        renderer.begin_shadow_pass(c, glm::value_ptr(vp));
        for (const auto& in : instances) {
          if (glm::dot(in.origin - cam, in.origin - cam) >
              (shadow_range + in.cull_radius) * (shadow_range + in.cull_radius))
            continue;
          const auto mesh_it = template_cache.find(in.mesh_key);
          if (mesh_it == template_cache.end() || mesh_it->second.vao == 0) continue;
          renderer.draw_depth(mesh_it->second, glm::value_ptr(in.model));
        }
        for (const auto& v : vehicles) {
          if (v.destroyed) continue;
          if (glm::dot(v.origin - cam, v.origin - cam) > shadow_cast_dist2) continue;
          const auto vit = vehicle_cache.find(v.mesh_key);
          if (vit != vehicle_cache.end() && vit->second.vao != 0)
            renderer.draw_depth(vit->second, glm::value_ptr(v.model));
          for (const auto& part : v.parts) {
            const auto pit = vehicle_cache.find(part.mesh_key);
            if (pit == vehicle_cache.end() || pit->second.vao == 0) continue;
            renderer.draw_depth(pit->second, glm::value_ptr(v.model * part.local));
          }
        }
      }
      renderer.set_viewport(cur_w, cur_h);
    }
    renderer.set_shadows(cascade_vp, cascade_splits, settings.shadows_enabled);

    const float frame_t =
        static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
    const float rs = std::clamp(settings.render_scale, 0.5f, 1.f);
    const int scene_w = std::max(320, static_cast<int>(std::lround(cur_w * rs)));
    const int scene_h = std::max(180, static_cast<int>(std::lround(cur_h * rs)));
    renderer.begin_scene(scene_w, scene_h, atmo.horizon_color.x, atmo.horizon_color.y,
                         atmo.horizon_color.z);
    // Mirrored projection flips triangle winding — treat CW as front for the world.
    glFrontFace(GL_CW);

    // Per-frame environment: camera position + sun/moon + horizon fog. Fog fades far
    // terrain/objects/water into the horizon colour so map edges disappear.
    glm::vec3 light_dir = atmo.sun_dir;
    glm::vec3 light_color = atmo.sun_color;
    glm::vec3 env_fog = atmo.fog_color;
    float ambient_scale = 1.f;
    if (atmo.is_night) {
      // Moon: flareDirection points at the moon; negate so L = -light_dir matches day.
      if (atmo.has_moon) light_dir = -atmo.moon_dir;
      light_color = glm::max(atmo.sun_color, glm::vec3(0.25f, 0.32f, 0.55f));
      light_color *= 0.55f;
      env_fog = glm::mix(atmo.horizon_color, atmo.fog_color, 0.5f);
      ambient_scale = 0.45f;
    }
    renderer.set_environment(glm::value_ptr(cam), glm::value_ptr(light_dir),
                             glm::value_ptr(env_fog), atmo.fog_start, atmo.fog_end,
                             glm::value_ptr(light_color), ambient_scale);

    glm::vec3 draw_sky_col = atmo.sky_color;
    glm::vec3 draw_horizon = atmo.horizon_color;
    const float sky_t =
        static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
    const glm::vec3 moon_dir =
        atmo.has_moon ? atmo.moon_dir
                      : (atmo.is_night ? glm::normalize(-atmo.sun_dir) : glm::vec3(0.f));
    const glm::vec3 moon_col =
        atmo.is_night ? glm::vec3(0.75f, 0.82f, 1.05f) : glm::vec3(0.f);
    renderer.draw_sky(glm::value_ptr(inv_view_proj), glm::value_ptr(cam),
                      glm::value_ptr(draw_sky_col), glm::value_ptr(draw_horizon), sky_tex,
                      cloud_tex, atmo.cloud_scroll.x * sky_t, atmo.cloud_scroll.y * sky_t,
                      atmo.is_night ? 0.18f : 0.55f, glm::value_ptr(atmo.sun_color),
                      atmo.is_night ? 1 : 0, glm::value_ptr(moon_dir),
                      glm::value_ptr(moon_col), atmo.dome_rotation);

    if (have_ground_tex) {
      // Repeat the detail grit roughly every ~6 m of world space.
      const float world_m = static_cast<float>(level.terrain.width) * xz;
      bf2::Renderer::TerrainDraw td;
      td.colormap = ground_tex.colormap;
      td.lightmap = ground_tex.lightmap;
      td.mask1 = ground_tex.mask1;
      td.mask2 = ground_tex.mask2;
      td.detail0 = ground_tex.detail0;
      td.detail1 = ground_tex.detail1;
      td.detail2 = ground_tex.detail2;
      td.detail_tiling = world_m > 0.f ? world_m / 6.f : 64.f;
      renderer.draw_terrain_colormap(terrain_tex_gpu, glm::value_ptr(view_proj), td);
    } else {
      renderer.draw_mesh(terrain_gpu, glm::value_ptr(view_proj));
    }

    int drawn = 0;
    for (const auto& in : instances) {
      const glm::vec3 d = in.origin - cam;
      // Sphere cull: keep tall props (cranes) when the pivot is past draw_dist.
      const float cull = draw_dist + in.cull_radius;
      if (glm::dot(d, d) > cull * cull) {
        continue;
      }
      const auto mesh_it = template_cache.find(in.mesh_key);
      if (mesh_it == template_cache.end() || mesh_it->second.vao == 0) {
        continue;
      }
      const glm::mat4 mvp = view_proj * in.model;
      renderer.draw_textured(mesh_it->second, glm::value_ptr(mvp), glm::value_ptr(in.model),
                             in.lightmap, glm::value_ptr(in.lm_xform), false, in.alpha_mode);
      ++drawn;
    }

    // Vehicles (static props at their spawn points): body + attached child parts.
    const auto apply_gear_tuck = [](const glm::mat4& rest, float anim, float angle, int axis) {
      if (anim <= 0.0005f) return rest;
      const float rad = glm::radians(-anim * angle);
      glm::mat4 tuck(1.f);
      if (axis == 0)
        tuck = glm::rotate(tuck, rad, glm::vec3(0, 1, 0));
      else if (axis == 2)
        tuck = glm::rotate(tuck, rad, glm::vec3(0, 0, 1));
      else
        tuck = glm::rotate(tuck, rad, glm::vec3(1, 0, 0));
      return rest * tuck;
    };
    const auto track_uv_scroll = [](float spin, float speed) {
      const float sign = speed >= 0.f ? 1.f : -1.f;
      float u = sign * spin * 0.08f;
      u = u - std::floor(u);  // wrap to [0,1) for stable tiling
      if (u < 0.f) u += 1.f;
      return u;
    };
    for (const auto& v : vehicles) {
      if (v.destroyed) continue;
      const glm::vec3 d = v.pos - cam;
      if (glm::dot(d, d) > draw_dist2) continue;
      const auto vit = vehicle_cache.find(v.mesh_key);
      if (vit != vehicle_cache.end() && vit->second.vao != 0) {
        const glm::mat4 mvp = view_proj * v.model;
        const float track_scroll =
            v.is_tracked && !v.wheel_spin.empty()
                ? track_uv_scroll(v.wheel_spin[0], v.speed)
                : 0.f;
        // Cull back faces on the hull so you don't see the interior back-faces
        // through hatches/viewports (the "see inside the vehicle" artifact).
        renderer.draw_textured(vit->second, glm::value_ptr(mvp), glm::value_ptr(v.model), 0, nullptr,
                               true, 0, track_scroll);
        ++drawn;
      }
      const float gear_anim = (v.is_air && !v.is_heli) ? v.jet_gear_anim : 0.f;
      for (std::size_t wi = 0; wi < v.wheels.size(); ++wi) {
        const auto& wslot = v.wheels[wi];
        const auto wit = vehicle_cache.find(wslot.mesh_key);
        if (wit == vehicle_cache.end() || wit->second.vao == 0) continue;
        glm::mat4 pm = v.model * apply_gear_tuck(wslot.rest, gear_anim, wslot.gear_tuck_angle,
                                                 wslot.gear_tuck_axis);
        if (wslot.steers) pm = glm::rotate(pm, v.visual_steer, glm::vec3(0, 1, 0));
        const float spin = wi < v.wheel_spin.size() ? v.wheel_spin[wi] : 0.f;
        if (wslot.spin_geometry) pm = glm::rotate(pm, spin, glm::vec3(1, 0, 0));
        const glm::mat4 mvp = view_proj * pm;
        renderer.draw_textured(wit->second, glm::value_ptr(mvp), glm::value_ptr(pm), 0, nullptr,
                               false, 0, 0.f, false);
        ++drawn;
      }
      for (const auto& gslot : v.gear_parts) {
        const auto git = vehicle_cache.find(gslot.mesh_key);
        if (git == vehicle_cache.end() || git->second.vao == 0) continue;
        const glm::mat4 pm =
            v.model * apply_gear_tuck(gslot.rest, gear_anim, gslot.gear_tuck_angle, gslot.gear_tuck_axis);
        const glm::mat4 mvp = view_proj * pm;
        renderer.draw_textured(git->second, glm::value_ptr(mvp), glm::value_ptr(pm));
        ++drawn;
      }
      for (const auto& part : v.parts) {
        const bool is_rotor = v.is_air && part.mesh_key.find("rotor") != std::string::npos;
        glm::mat4 pm = v.model * part.local;
        if (is_rotor) {
          // Spin the rotor by its current (RPM-scaled) angle, then pick the
          // representation: static blades (geom0) while parked/spooling, blur
          // disc (geom1) once it winds up. Crossfade near the switch so it reads
          // as the blades "disappearing" into a blur, like a real rotor.
          pm = glm::rotate(pm, v.rotor_spin, glm::vec3(0, 1, 0));
          const glm::mat4 mvp = view_proj * pm;
          const float rpm = v.rotor_rpm;
          const auto bit = vehicle_cache.find(part.mesh_key);
          const auto blit = vehicle_cache.find(part.mesh_key + "#blur");
          if (rpm < 0.75f && bit != vehicle_cache.end() && bit->second.vao != 0) {
            renderer.draw_textured(bit->second, glm::value_ptr(mvp), glm::value_ptr(pm));
            ++drawn;
          }
          if (rpm > 0.45f && blit != vehicle_cache.end() && blit->second.vao != 0) {
            renderer.draw_textured(blit->second, glm::value_ptr(mvp), glm::value_ptr(pm), 0, nullptr,
                                   false, 1);
            ++drawn;
          }
          continue;
        }
        const auto pit = vehicle_cache.find(part.mesh_key);
        if (pit == vehicle_cache.end() || pit->second.vao == 0) continue;
        const glm::mat4 mvp = view_proj * pm;
        renderer.draw_textured(pit->second, glm::value_ptr(mvp), glm::value_ptr(pm));
        ++drawn;
      }
    }

    // MIM-23 Hawk emplacement — open ground by Chinese Z-8 (off runway).
    if (hawk_present && hawk_mesh.vao != 0) {
      const glm::mat4 model =
          glm::translate(glm::mat4(1.f), hawk_pos) *
          glm::rotate(glm::mat4(1.f), glm::radians(hawk_heading), glm::vec3(0.f, 1.f, 0.f)) *
          glm::scale(glm::mat4(1.f), glm::vec3(hawk_draw_scale)) *
          glm::translate(glm::mat4(1.f), glm::vec3(0.f, hawk_ground_lift, 0.f)) * hawk_mesh_fix;
      const glm::mat4 mvp = view_proj * model;
      renderer.draw_textured(hawk_mesh, glm::value_ptr(mvp), glm::value_ptr(model));
    }

    // Third-person animated soldier: pick a locomotion clip by movement speed and
    // drive the GPU skinning palette; body is placed at the player's feet facing
    // the look direction.
    if (third_person && have_soldier && in_vehicle < 0) {
      const float spd = std::sqrt(player.desired_velocity.x * player.desired_velocity.x +
                                  player.desired_velocity.z * player.desired_velocity.z);
      const bool moving = spd > 0.3f;
      const bool running = spd > 8.0f;
      const dalian::SoldierPose pose = game_sim.state().infantry_pose;
      const bf2::AnimationClip* clip = nullptr;
      if (pose == dalian::SoldierPose::Prone && soldier_anims.prone) {
        clip = soldier_anims.prone;
      } else if (pose == dalian::SoldierPose::Crouch && soldier_anims.crouch) {
        clip = soldier_anims.crouch;
      } else if (running && soldier_anims.run) {
        clip = soldier_anims.run;
      } else if (moving && soldier_anims.walk) {
        clip = soldier_anims.walk;
      } else if (soldier_anims.stand) {
        clip = soldier_anims.stand;
      }

      constexpr float kAnimFps = 30.f;
      int frame = 0;
      if (clip && clip->frame_count > 0) {
        frame = static_cast<int>(anim_time * kAnimFps) % clip->frame_count;
      }
      const auto palette = bf2::compute_skin_palette(soldier_src, soldier_ske, clip, frame, 1, 0);

      const float feet_y = player.position.y - player.eye_height;
      const float facing = std::atan2(flat_front.x, flat_front.z);
      glm::mat4 body(1.0f);
      body = glm::translate(body, glm::vec3(player.position.x, feet_y, player.position.z));
      body = glm::rotate(body, facing, glm::vec3(0, 1, 0));
      const glm::mat4 body_mvp = view_proj * body;
      if (!palette.empty()) {
        renderer.draw_skinned(soldier_mesh, glm::value_ptr(body_mvp), glm::value_ptr(palette[0]),
                              static_cast<int>(palette.size()), soldier_tex, glm::value_ptr(body));
      }

      // Held weapon: the same rifle shown in first person, placed at the soldier's
      // right hand height pointing forward. (Body-relative, not hand-bone locked;
      // a good approximation of the "at ready" pose.)
      if (have_weapon_model && weapon_meshes[weapon_index].vao != 0) {
        glm::mat4 held = body;
        held = glm::translate(held, glm::vec3(0.16f, 1.32f, 0.18f));
        held = glm::rotate(held, glm::radians(180.f), glm::vec3(0, 1, 0));  // barrel -> +Z
        const glm::mat4 held_mvp = view_proj * held;
        renderer.draw_textured(weapon_meshes[weapon_index], glm::value_ptr(held_mvp),
                               glm::value_ptr(held));
      }
    }

    // Enemy soldiers: skinned bodies at their feet, facing the player. Dead
    // bodies topple forward and sink into the ground before respawning.
    if (have_soldier) {
      for (auto& en : enemies) {
        const glm::vec3 d = en.pos - cam;
        if (glm::dot(d, d) > draw_dist2) continue;
        dalian::SoldierAnimState ast;
        ast.alive = en.alive;
        ast.moving = en.moving;
        ast.move_speed = en.moving ? 2.8f : 0.f;
        ast.pose = !en.alive ? dalian::SoldierPose::Death : dalian::SoldierPose::Stand;
        ast.time = en.anim_time;
        float anim_rate = 1.f;
        const bf2::AnimationClip* clip =
            dalian::select_soldier_clip(soldier_anims, ast, anim_rate);
        int frame = 0;
        if (clip && clip->frame_count > 0) {
          frame = static_cast<int>(en.anim_time * 30.f * anim_rate) % clip->frame_count;
        }
        // Skin against the opfor mesh's own bind when present, else the US body.
        const bf2::Mesh& esrc = have_enemy_mesh ? enemy_src : soldier_src;
        const auto palette = bf2::compute_skin_palette(esrc, soldier_ske, clip, frame, 1, 0);
        if (palette.empty()) continue;
        glm::mat4 body(1.0f);
        if (!en.alive) {
          const float fall = std::min(en.death_time, 1.0f);
          const float sink = std::min(en.death_time * 0.4f, 0.6f);
          body = glm::translate(body, en.pos - glm::vec3(0.f, sink, 0.f));
          body = glm::rotate(body, en.yaw, glm::vec3(0, 1, 0));
          body = glm::rotate(body, glm::radians(90.f) * fall, glm::vec3(1, 0, 0));
        } else {
          body = glm::translate(body, en.pos);
          body = glm::rotate(body, en.yaw, glm::vec3(0, 1, 0));
        }
        const glm::mat4 body_mvp = view_proj * body;
        // Opfor body + texture when available; otherwise the US body reddened so
        // friend/foe still reads. A hit blends toward bright red for feedback.
        const bf2::GpuSkinnedMesh& emesh = have_enemy_mesh ? enemy_mesh : soldier_mesh;
        const std::uint32_t etex = have_enemy_mesh ? enemy_tex : soldier_tex;
        glm::vec3 tint = have_enemy_mesh ? glm::vec3(1.f) : glm::vec3(1.15f, 0.72f, 0.66f);
        if (en.hit_flash > 0.f) tint = glm::mix(tint, glm::vec3(2.0f, 0.4f, 0.4f),
                                                std::clamp(en.hit_flash * 3.f, 0.f, 1.f));
        renderer.draw_skinned(emesh, glm::value_ptr(body_mvp), glm::value_ptr(palette[0]),
                              static_cast<int>(palette.size()), etex, glm::value_ptr(body),
                              glm::value_ptr(tint));
      }
    }

    // Remote players (multiplayer): draw each other client's soldier at its
    // smoothed transform, animated by the replicated move state, tinted bluish
    // so teammates read distinctly from the red opfor.
    if (have_soldier && net.active()) {
      for (const auto& rp : net.players()) {
        if (!rp.active || rp.id == net.local_id()) continue;
        if (rp.vehicle_id >= 0) continue;  // body hidden while driving; vehicle mesh shows instead
        const glm::vec3 rpos(rp.rx, rp.ry, rp.rz);
        if (glm::dot(rpos - cam, rpos - cam) > draw_dist2) continue;

        const float rspd = std::sqrt(rp.vx * rp.vx + rp.vz * rp.vz);
        dalian::SoldierAnimState ast;
        ast.alive = rp.health > 0;
        ast.pose = static_cast<dalian::SoldierPose>(rp.pose);
        ast.moving = rspd > 0.3f || rp.anim > 0;
        ast.move_speed = rspd;
        ast.time = rp.ranim_time;
        float anim_rate = 1.f;
        const bf2::AnimationClip* clip =
            dalian::select_soldier_clip(soldier_anims, ast, anim_rate);
        if (!clip && rp.anim == 2 && soldier_anims.run)
          clip = soldier_anims.run;
        else if (!clip && rp.anim == 1 && soldier_anims.walk)
          clip = soldier_anims.walk;
        else if (!clip && soldier_anims.stand)
          clip = soldier_anims.stand;

        int frame = 0;
        if (clip && clip->frame_count > 0) {
          frame = static_cast<int>(rp.ranim_time * 30.f * anim_rate) % clip->frame_count;
        }
        const auto palette = bf2::compute_skin_palette(soldier_src, soldier_ske, clip, frame, 1, 0);
        if (palette.empty()) continue;

        const float facing = glm::radians(rp.ryaw);

        glm::mat4 body = glm::translate(glm::mat4(1.0f), rpos);
        body = glm::rotate(body, facing, glm::vec3(0, 1, 0));
        const glm::mat4 body_mvp = view_proj * body;
        const glm::vec3 tint(1.f);
        renderer.draw_skinned(soldier_mesh, glm::value_ptr(body_mvp), glm::value_ptr(palette[0]),
                              static_cast<int>(palette.size()), soldier_tex, glm::value_ptr(body),
                              glm::value_ptr(tint));
        if (have_weapon_model && weapon_meshes[weapon_index].vao != 0) {
          glm::mat4 held = glm::translate(body, glm::vec3(0.16f, 1.32f, 0.18f));
          held = glm::rotate(held, glm::radians(180.f), glm::vec3(0, 1, 0));
          const glm::mat4 held_mvp = view_proj * held;
          renderer.draw_textured(weapon_meshes[weapon_index], glm::value_ptr(held_mvp),
                                 glm::value_ptr(held));
        }
      }
    }

    // Grass: alpha-tested billboards scattered on the ground near the player.
    // Rebuilt only when the player has walked far enough from the last patch.
    if (undergrowth.valid() && grass_atlas_tex != 0 && settings.grass_enabled) {
      grass_radius = settings.grass_distance;
      const float t_sec =
          static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
      // Rebuild after moving a fraction of the radius: keeps the patch centred
      // on the player while avoiding a costly rebuild every few metres.
      const float grass_step = std::max(6.f, grass_radius * 0.25f);
      if (std::abs(cam.x - grass_center.x) > grass_step ||
          std::abs(cam.z - grass_center.y) > grass_step) {
        const float wlvl = atmo.has_water ? atmo.water_level : -1e9f;
        bf2::build_grass_vertices(
            undergrowth, [&](float x, float z) { return world.terrain_height(x, z); }, cam.x, cam.z,
            grass_radius, wlvl, grass_verts);
        grass_center = glm::vec2(cam.x, cam.z);
      }
      renderer.draw_grass(glm::value_ptr(view_proj), glm::value_ptr(cam), grass_atlas_tex,
                          grass_verts.data(), static_cast<int>(grass_verts.size() / 6), t_sec);
    }

    // Translucent water plane at sea level, centred on the camera so it always
    // reaches the horizon. Drawn after opaque geometry so blending is correct.
    if (atmo.has_water) {
      const float t_sec =
          static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
      const float height_above = cam.y - atmo.water_level;
      const float extent = std::max(2400.f, std::min(draw_dist * 0.55f,
                                                     height_above > 180.f ? 3200.f : 2800.f));
      renderer.draw_water(glm::value_ptr(view_proj), atmo.water_level, cam.x, cam.z, extent, t_sec,
                          glm::value_ptr(atmo.water_color), glm::value_ptr(atmo.water_specular),
                          atmo.water_specular_power, glm::value_ptr(atmo.water_fog_color),
                          atmo.water_sun_intensity, glm::value_ptr(light_color));
    }

    // World-space effects: tracers (mesh or lines) and bullet impacts.
    if (!tracers.empty()) {
      if (tracer_mesh.vao != 0) {
        for (const auto& t : tracers) {
          const glm::vec3 f = glm::normalize(t.b - t.a);
          glm::vec3 up0(0.f, 1.f, 0.f);
          if (std::fabs(glm::dot(f, up0)) > 0.99f) up0 = glm::vec3(1.f, 0.f, 0.f);
          const glm::vec3 r = glm::normalize(glm::cross(up0, f));
          const glm::vec3 u = glm::cross(f, r);
          glm::mat4 rot(1.f);
          rot[0] = glm::vec4(r, 0.f);
          rot[1] = glm::vec4(u, 0.f);
          rot[2] = glm::vec4(f, 0.f);
          const glm::vec3 mid = (t.a + t.b) * 0.5f;
          const float len = glm::length(t.b - t.a);
          glm::mat4 model = glm::translate(glm::mat4(1.f), mid) * rot;
          model = glm::scale(model, glm::vec3(0.04f, 0.04f, std::max(0.35f, len)));
          const glm::mat4 mvp = view_proj * model;
          renderer.draw_textured(tracer_mesh, glm::value_ptr(mvp), glm::value_ptr(model));
        }
      } else {
        std::vector<float> line_verts;
        line_verts.reserve(tracers.size() * 6);
        for (const auto& t : tracers) {
          line_verts.insert(line_verts.end(), {t.a.x, t.a.y, t.a.z, t.b.x, t.b.y, t.b.z});
        }
        renderer.draw_lines(glm::value_ptr(view_proj), line_verts.data(),
                            static_cast<int>(line_verts.size() / 3), 1.0f, 0.85f, 0.35f, 2.5f,
                            true);
      }
    }
    if (!impacts.empty()) {
      std::vector<float> imp_verts;
      imp_verts.reserve(impacts.size() * 18);
      const float s = 0.12f;
      for (const auto& im : impacts) {
        const glm::vec3 p = im.p;
        imp_verts.insert(imp_verts.end(), {p.x - s, p.y, p.z, p.x + s, p.y, p.z});
        imp_verts.insert(imp_verts.end(), {p.x, p.y - s, p.z, p.x, p.y + s, p.z});
        imp_verts.insert(imp_verts.end(), {p.x, p.y, p.z - s, p.x, p.y, p.z + s});
      }
      renderer.draw_lines(glm::value_ptr(view_proj), imp_verts.data(),
                          static_cast<int>(imp_verts.size() / 3), 0.9f, 0.9f, 0.7f, 2.0f, true);
    }
    // In-flight bullets: short bright streaks along each projectile's velocity.
    if (!projectiles.empty()) {
      std::vector<float> pr_verts;
      pr_verts.reserve(projectiles.size() * 6);
      for (const auto& pr : projectiles) {
        const glm::vec3 tail = pr.pos - glm::normalize(pr.vel) * 1.2f;
        pr_verts.insert(pr_verts.end(), {pr.pos.x, pr.pos.y, pr.pos.z, tail.x, tail.y, tail.z});
      }
      renderer.draw_lines(glm::value_ptr(view_proj), pr_verts.data(),
                          static_cast<int>(pr_verts.size() / 3), 1.0f, 0.95f, 0.55f, 2.5f, true);
    }

    // Missiles: draw the bundledmesh oriented along its flight path (nose = mesh
    // +Z). Car-SAM rounds are scaled up so the body reads at distance; also draw
    // a bright body streak so the round is never "just fireworks".
    if (!missiles.empty() && missile_mesh.vao != 0) {
      for (const auto& am : missiles) {
        const glm::vec3 f = am.m.forward();
        glm::vec3 up0(0.f, 1.f, 0.f);
        if (std::fabs(glm::dot(f, up0)) > 0.99f) up0 = glm::vec3(1.f, 0.f, 0.f);
        const glm::vec3 r = glm::normalize(glm::cross(up0, f));
        const glm::vec3 u = glm::cross(f, r);
        glm::mat4 rot(1.0f);
        rot[0] = glm::vec4(r, 0.f);
        rot[1] = glm::vec4(u, 0.f);
        rot[2] = glm::vec4(f, 0.f);
        const float sc = am.detonation_fx == 1 ? missile_draw_scale_sam : 1.f;
        const glm::mat4 model =
            glm::translate(glm::mat4(1.0f), am.m.position) * rot * glm::scale(glm::mat4(1.0f), glm::vec3(sc));
        const glm::mat4 mvp = view_proj * model;
        renderer.draw_textured(missile_mesh, glm::value_ptr(mvp), glm::value_ptr(model));
      }
    }
    if (!missiles.empty()) {
      std::vector<float> mv;
      mv.reserve(missiles.size() * 6);
      for (const auto& am : missiles) {
        const float len = am.detonation_fx == 1 ? 6.5f : 2.5f;
        const glm::vec3 a = am.m.position;
        const glm::vec3 b = a - am.m.forward() * len;
        mv.insert(mv.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
      }
      // Bright streak so the round reads even if the mesh is tiny / occluded by trail.
      renderer.draw_lines(glm::value_ptr(view_proj), mv.data(),
                          static_cast<int>(mv.size() / 3), 1.0f, 0.55f, 0.12f, 5.0f, true);
    }

    // BF2-style particle billboards (smoke, exhaust, explosions, baselights).
    {
      const float t_sec =
          static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
      const glm::vec3 bill_right = [&]() {
        glm::vec3 r = glm::cross(cam_up, cam_front);
        if (glm::dot(r, r) < 1e-8f) r = bf2_right(glm::vec3(0.f, 0.f, 1.f));
        return glm::normalize(r);
      }();
      const glm::vec3 bill_up = cam_up;
      std::vector<bf2::Renderer::BillboardParticle> smoke_parts;
      std::vector<bf2::Renderer::BillboardParticle> glow_parts;
      smoke_parts.reserve(smoke.size());
      glow_parts.reserve(scene_lights.size() + explosions.size() * 4 + 8);
      for (const auto& s : smoke) {
        const float k = s.life > 0.f ? glm::clamp(s.age / s.life, 0.f, 1.f) : 1.f;
        const float graph_a =
            s.use_graphs ? std::clamp(s.transp_graph.eval(k), 0.f, 1.f) : (1.f - k);
        const float alpha = graph_a * (s.kind >= 2 ? 0.95f : 0.75f);
        bf2::Renderer::BillboardParticle bp{};
        bp.x = s.p.x;
        bp.y = s.p.y;
        bp.z = s.p.z;
        bp.size = s.size * (s.use_graphs ? 1.f : (1.f + k * 1.8f));
        bp.kind = static_cast<float>(s.kind);
        if (glm::length(s.tint) > 0.01f) {
          bp.r = s.tint.r;
          bp.g = s.tint.g;
          bp.b = s.tint.b;
        } else if (s.kind == 2) {
          bp.r = 1.f;
          bp.g = 0.55f;
          bp.b = 0.12f;
        } else if (s.kind == 3) {
          bp.r = 1.f;
          bp.g = 0.7f;
          bp.b = 0.2f;
        } else if (s.kind == 1) {
          bp.r = 0.85f;
          bp.g = 0.85f;
          bp.b = 0.8f;
        } else {
          bp.r = 0.55f;
          bp.g = 0.56f;
          bp.b = 0.58f;
        }
        bp.a = alpha;
        if (s.kind >= 2)
          glow_parts.push_back(bp);
        else
          smoke_parts.push_back(bp);
      }
      for (const auto& sl : scene_lights) {
        // Soft radial glow (camera-facing) — not a world-aligned streak.
        const float pulse = 0.88f + 0.12f * std::sin(t_sec * 3.5f + sl.pos.x * 0.07f);
        bf2::Renderer::BillboardParticle bp{};
        bp.x = sl.pos.x;
        bp.y = sl.pos.y + 0.35f;
        bp.z = sl.pos.z;
        bp.size = std::clamp(sl.radius * 0.12f, 1.2f, 4.5f) * pulse;
        bp.r = sl.color.r;
        bp.g = sl.color.g;
        bp.b = sl.color.b;
        bp.a = 0.42f * pulse;
        bp.kind = 2.f;
        glow_parts.push_back(bp);
        // Inner hot core
        bf2::Renderer::BillboardParticle core = bp;
        core.size *= 0.28f;
        core.a = 0.75f * pulse;
        core.r = std::min(1.f, sl.color.r * 1.15f);
        core.g = std::min(1.f, sl.color.g * 1.1f);
        core.b = std::min(1.f, sl.color.b * 1.05f);
        glow_parts.push_back(core);
      }
      for (const auto& ex : explosions) {
        const float k = ex.life > 0.f ? glm::clamp(ex.age / ex.life, 0.f, 1.f) : 1.f;
        const float alpha = 1.f - k;
        for (int layer = 0; layer < 3; ++layer) {
          bf2::Renderer::BillboardParticle bp{};
          bp.x = ex.p.x;
          bp.y = ex.p.y + layer * 0.4f;
          bp.z = ex.p.z;
          bp.size = (2.4f + k * 14.f) * ex.scale * (1.f + layer * 0.3f);
          bp.r = 1.f;
          bp.g = 0.45f + layer * 0.1f;
          bp.b = 0.08f;
          bp.a = alpha * (0.9f - layer * 0.2f);
          bp.kind = 3.f;
          glow_parts.push_back(bp);
        }
      }
      const std::uint32_t glow_tex = fx_flash_tex ? fx_flash_tex : fx_fire_tex;
      if (!smoke_parts.empty() && fx_smoke_tex) {
        renderer.draw_billboards(glm::value_ptr(view_proj), glm::value_ptr(bill_right),
                                 glm::value_ptr(bill_up), smoke_parts.data(),
                                 static_cast<int>(smoke_parts.size()), fx_smoke_tex, true, false);
      }
      if (!glow_parts.empty() && (glow_tex || fx_fire_tex)) {
        renderer.draw_billboards(glm::value_ptr(view_proj), glm::value_ptr(bill_right),
                                 glm::value_ptr(bill_up), glow_parts.data(),
                                 static_cast<int>(glow_parts.size()),
                                 glow_tex ? glow_tex : fx_fire_tex, true, true);
      }
    }

    // First-person weapon viewmodel: rendered in view space over the scene with a
    // narrower FOV so it doesn't distort. The depth buffer is cleared first so
    // the weapon never clips into the world but still self-occludes correctly.
    if (!third_person && !drone_mode && in_vehicle < 0) {
      glFrontFace(GL_CCW);  // viewmodel uses unmirrored view-space projection
      const glm::mat4 vm_proj = glm::perspective(
          glm::radians(55.f), static_cast<float>(cur_w) / std::max(cur_h, 1), 0.01f, 50.f);
      // Base placement tuned via the snapshot --viewmodel preview.
      glm::mat4 vm(1.0f);
      // Recoil: kick back (+z toward viewer) and tilt the muzzle up.
      vm = glm::translate(vm, glm::vec3(0.f, recoil * 0.012f, recoil * 0.06f));
      vm = glm::rotate(vm, recoil * -0.10f, glm::vec3(1, 0, 0));

      glClear(GL_DEPTH_BUFFER_BIT);
      if (have_weapon_model && weapon_meshes[weapon_index].vao != 0) {
        glm::mat4 gun_model = vm;
        gun_model = glm::translate(gun_model, glm::vec3(0.14f, -0.145f, -0.46f));
        // BF2 bundledmesh barrels point +Z (toward the viewer) in their native
        // frame, so flip 180 deg about Y to aim the muzzle down-range, then add a
        // small yaw so it reads as held slightly across the body.
        gun_model = glm::rotate(gun_model, glm::radians(180.f + 7.f), glm::vec3(0, 1, 0));
        const glm::mat4 gun_mvp = vm_proj * gun_model;
        renderer.draw_textured(weapon_meshes[weapon_index], glm::value_ptr(gun_mvp),
                               glm::value_ptr(gun_model));
      } else {
        const glm::mat4 gun_mvp = vm_proj * vm;
        renderer.draw_color(gun_mesh, glm::value_ptr(gun_mvp), glm::value_ptr(vm), 0.15f, 0.16f,
                            0.18f, true, true);
      }

      // Muzzle flash: soft additive sprite at the barrel tip (not a line-star).
      if (muzzle_flash > 0.f) {
        glm::mat4 flash_xf = vm;
        if (have_weapon_model && weapon_meshes[weapon_index].vao != 0) {
          flash_xf = glm::translate(flash_xf, glm::vec3(0.14f, -0.145f, -0.46f));
          flash_xf = glm::rotate(flash_xf, glm::radians(180.f + 7.f), glm::vec3(0, 1, 0));
        }
        const glm::vec3 tip_local = have_weapon_model ? glm::vec3(0.05f, 0.02f, 0.95f)
                                                      : glm::vec3(0.17f, -0.10f, -1.25f);
        const glm::vec3 tip = glm::vec3(flash_xf * glm::vec4(tip_local, 1.f));
        const float f = 0.08f + muzzle_flash * 2.4f;
        const std::uint32_t flash_tex = fx_flash_tex ? fx_flash_tex : fx_fire_tex;
        if (flash_tex) {
          bf2::Renderer::BillboardParticle flash[2]{};
          flash[0].x = tip.x;
          flash[0].y = tip.y;
          flash[0].z = tip.z;
          flash[0].size = f;
          flash[0].r = 1.f;
          flash[0].g = 0.92f;
          flash[0].b = 0.55f;
          flash[0].a = std::clamp(muzzle_flash * 18.f, 0.f, 1.f);
          flash[0].kind = 2.f;
          flash[1] = flash[0];
          flash[1].size = f * 0.45f;
          flash[1].a *= 0.9f;
          flash[1].r = 1.f;
          flash[1].g = 1.f;
          flash[1].b = 0.85f;
          const float vr[3] = {1.f, 0.f, 0.f};
          const float vu[3] = {0.f, 1.f, 0.f};
          renderer.draw_billboards(glm::value_ptr(vm_proj), vr, vu, flash, 2, flash_tex, true,
                                   true);
        }
      }
    }

    // Resolve offscreen HDR scene (tone map + bloom + SSAO). Drone mode adds FPV degrade.
    {
      const float degrade = drone_mode ? (1.f - signal) : 0.f;
      renderer.present_scene(degrade, frame_t, 0.2f, far_z, cur_w, cur_h);
    }

    // Screen-space HUD (orthographic).
    {
      const glm::mat4 ortho = glm::ortho(0.f, static_cast<float>(cur_w), 0.f,
                                         static_cast<float>(cur_h), -1.f, 1.f);
      const float cx = cur_w * 0.5f;
      const float cy = cur_h * 0.5f;

      if (drone_mode) {
        // The video-feed degradation (static/scanlines/tearing/chromatic) is done
        // in the present_scene() post-process; here we only draw the crisp HUD.
        // Centre reticle — red for kamikaze (terminal munition), green for recon.
        const float b = 14.f;
        const float rr = kamikaze_mode ? 1.0f : 0.3f;
        const float gg = kamikaze_mode ? 0.15f : 1.0f;
        const float bb = kamikaze_mode ? 0.1f : 0.4f;
        std::vector<float> box = {cx - b, cy - b, 0.f, cx + b, cy - b, 0.f, cx + b, cy - b, 0.f,
                                  cx + b, cy + b, 0.f, cx + b, cy + b, 0.f, cx - b, cy + b, 0.f,
                                  cx - b, cy + b, 0.f, cx - b, cy - b, 0.f};
        renderer.draw_lines(glm::value_ptr(ortho), box.data(), 8, rr, gg, bb, 1.5f, false);
        if (kamikaze_mode) {
          // Corner brackets hint: LMB/Space = terminal detonation.
          const float c = 22.f, s = 8.f;
          std::vector<float> corners = {
              cx - c, cy - c, 0.f, cx - c + s, cy - c, 0.f, cx - c, cy - c, 0.f, cx - c, cy - c + s,
              0.f, cx + c, cy - c, 0.f, cx + c - s, cy - c, 0.f, cx + c, cy - c, 0.f, cx + c,
              cy - c + s, 0.f, cx - c, cy + c, 0.f, cx - c + s, cy + c, 0.f, cx - c, cy + c, 0.f,
              cx - c, cy + c - s, 0.f, cx + c, cy + c, 0.f, cx + c - s, cy + c, 0.f, cx + c,
              cy + c, 0.f, cx + c, cy + c - s, 0.f,
          };
          renderer.draw_lines(glm::value_ptr(ortho), corners.data(),
                              static_cast<int>(corners.size() / 3), 1.0f, 0.2f, 0.1f, 2.0f, false);
        }

        // Telemetry bars: battery (left) and throttle (right).
        auto bar = [&](float x, float frac, float r, float g, float bl) {
          const float w = 200.f, y = 30.f;
          std::vector<float> bg = {x, y, 0.f, x + w, y, 0.f};
          renderer.draw_lines(glm::value_ptr(ortho), bg.data(), 2, 0.1f, 0.1f, 0.1f, 12.f, false);
          if (frac > 0.f) {
            std::vector<float> fg = {x, y, 0.f, x + w * std::clamp(frac, 0.f, 1.f), y, 0.f};
            renderer.draw_lines(glm::value_ptr(ortho), fg.data(), 2, r, g, bl, 9.f, false);
          }
        };
        bar(24.f, drone.battery, drone.battery < 0.25f ? 0.9f : 0.2f,
            drone.battery < 0.25f ? 0.2f : 0.9f, 0.2f);        // battery
        bar(cur_w - 224.f, drone_throttle, 0.3f, 0.6f, 1.0f);  // throttle
      } else {
        // Rifle crosshair.
        const float g = 4.f;
        const float l = 10.f;
        std::vector<float> cross = {
            cx - g - l, cy, 0.f, cx - g, cy, 0.f, cx + g, cy, 0.f, cx + g + l, cy, 0.f,
            cx, cy - g - l, 0.f, cx, cy - g, 0.f, cx, cy + g, 0.f, cx, cy + g + l, 0.f,
        };
        renderer.draw_lines(glm::value_ptr(ortho), cross.data(), static_cast<int>(cross.size() / 3),
                            0.9f, 0.95f, 0.9f, 1.5f, false);
      }
    }

    // ---- Text HUD + deploy screen (uses the 2D UI with a real font) ---------
    {
      renderer.begin_ui(window);
      constexpr float W = 1600.f, H = 900.f;

      // Kill feed + join toasts (top-right).
      hud_feed.draw(renderer, W, H);

      if (settings.show_fps) {
        char fps_buf[48];
        std::snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", fps_smooth);
        const float tw = renderer.ui_text_width(fps_buf, 1.5f);
        renderer.ui_rect(16.f, 12.f, tw + 16.f, 28.f, 0.04f, 0.05f, 0.06f, 0.72f);
        renderer.ui_text(24.f, 18.f, 1.5f, fps_buf, 0.95f, 0.85f, 0.35f, 1.f);
      }

      if (deploy_open) {
        int lmx = 0, lmy = 0;
        SDL_GetMouseState(&lmx, &lmy);
        run_deploy_ui(lmx, lmy, deploy_click);
      } else if (sam_map_open) {
        int lmx = 0, lmy = 0;
        SDL_GetMouseState(&lmx, &lmy);
        float dx = 0.f, dy = 0.f;
        dalian::ui_mouse_design(renderer, lmx, lmy, dx, dy);

        renderer.ui_rect(0, 0, W, H, 0.03f, 0.04f, 0.06f, 0.88f);
        renderer.ui_text(40.f, 28.f, 2.4f, "CAR SAM  —  SELECT IMPACT", 0.95f, 0.75f, 0.25f, 1.f);
        renderer.ui_text(40.f, 62.f, 1.25f,
                         "Click anywhere on the map to fire from the nearest vehicle.  Esc / F8 cancel.",
                         0.7f, 0.74f, 0.78f, 1.f);

        const float map_x = 80.f, map_y = 100.f;
        const float map_w = W - 160.f, map_h = H - 180.f;
        renderer.ui_rect(map_x, map_y, map_w, map_h, 0.06f, 0.09f, 0.08f, 0.98f);
        renderer.ui_rect(map_x, map_y, map_w, 3.f, 0.35f, 0.55f, 0.7f, 1.f);

        glm::vec2 mn(1e30f), mxb(-1e30f);
        auto expand = [&](float x, float z) {
          mn.x = std::min(mn.x, x);
          mn.y = std::min(mn.y, z);
          mxb.x = std::max(mxb.x, x);
          mxb.y = std::max(mxb.y, z);
        };
        expand(player.position.x, player.position.z);
        for (const auto& cp : conquest_points) expand(cp.pos.x, cp.pos.z);
        for (const auto& v : vehicles) {
          if (v.mesh_key.find("vehicles/") != std::string::npos) expand(v.pos.x, v.pos.z);
        }
        for (const auto& m : deploy_markers) expand(m.map_pos.x, m.map_pos.z);
        glm::vec2 span = mxb - mn;
        if (span.x < 120.f) {
          const float c = 0.5f * (mn.x + mxb.x);
          mn.x = c - 60.f;
          mxb.x = c + 60.f;
        }
        if (span.y < 120.f) {
          const float c = 0.5f * (mn.y + mxb.y);
          mn.y = c - 60.f;
          mxb.y = c + 60.f;
        }
        const float pad = 28.f;
        dalian::MinimapProjector sam_map;
        sam_map.configure(mn, mxb, map_x + pad, map_y + pad, map_w - 2.f * pad, map_h - 2.f * pad);

        // Grid
        for (int i = 1; i < 8; ++i) {
          const float gx = map_x + pad + (map_w - 2.f * pad) * (static_cast<float>(i) / 8.f);
          const float gy = map_y + pad + (map_h - 2.f * pad) * (static_cast<float>(i) / 8.f);
          renderer.ui_rect(gx, map_y + pad, 1.f, map_h - 2.f * pad, 0.12f, 0.16f, 0.14f, 0.6f);
          renderer.ui_rect(map_x + pad, gy, map_w - 2.f * pad, 1.f, 0.12f, 0.16f, 0.14f, 0.6f);
        }

        auto team_rgb = [](dalian::TeamId t, float& r, float& g, float& b) {
          if (t == dalian::TeamId::Team1) {
            r = 0.45f;
            g = 0.7f;
            b = 1.f;
          } else if (t == dalian::TeamId::Team2) {
            r = 0.95f;
            g = 0.35f;
            b = 0.3f;
          } else {
            r = 0.55f;
            g = 0.55f;
            b = 0.55f;
          }
        };
        for (const auto& cp : conquest_points) {
          const glm::vec2 c = sam_map.world_to_minimap(cp.pos);
          float rr, rg, rb;
          team_rgb(cp.owner, rr, rg, rb);
          renderer.ui_rect(c.x - 7.f, c.y - 7.f, 14.f, 14.f, rr, rg, rb, 1.f);
          dalian::draw_clipped_text(renderer, c.x + 10.f, c.y - 6.f, 140.f, 1.05f, cp.name.c_str(),
                                    0.85f, 0.88f, 0.9f, 0.95f);
        }
        for (const auto& v : vehicles) {
          if (v.is_air || v.destroyed || v.mesh_key.find("vehicles/") == std::string::npos) continue;
          const glm::vec2 c = sam_map.world_to_minimap(v.pos);
          renderer.ui_rect(c.x - 4.f, c.y - 4.f, 8.f, 8.f, 0.95f, 0.85f, 0.2f, 1.f);
        }
        {
          const glm::vec2 p = sam_map.world_to_minimap(
              glm::vec3(player.position.x, player.position.y, player.position.z));
          renderer.ui_rect(p.x - 5.f, p.y - 5.f, 10.f, 10.f, 0.2f, 1.f, 0.45f, 1.f);
          renderer.ui_text(p.x + 10.f, p.y - 6.f, 1.1f, "YOU", 0.3f, 1.f, 0.5f, 1.f);
        }
        // Cursor crosshair on map
        if (sam_map.contains_screen(dx, dy)) {
          renderer.ui_rect(dx - 10.f, dy - 1.f, 20.f, 2.f, 1.f, 0.55f, 0.15f, 1.f);
          renderer.ui_rect(dx - 1.f, dy - 10.f, 2.f, 20.f, 1.f, 0.55f, 0.15f, 1.f);
        }
      } else if (!drone_mode && in_vehicle < 0) {
        char buf[96];
        // Bottom-left: health + stamina bars.
        const float bx = 24.f, bw = 240.f, bh = 14.f;
        const float hy = H - 40.f, syb = H - 62.f;
        renderer.ui_rect(bx, hy, bw, bh, 0.10f, 0.10f, 0.10f, 0.75f);
        const float hf = std::clamp(player_health / 100.f, 0.f, 1.f);
        renderer.ui_rect(bx, hy, bw * hf, bh, hf < 0.35f ? 0.85f : 0.2f, hf < 0.35f ? 0.2f : 0.8f,
                         0.25f, 0.95f);
        renderer.ui_rect(bx, syb, bw, bh - 4, 0.10f, 0.10f, 0.10f, 0.75f);
        const float sf = std::clamp(player_stamina / 100.f, 0.f, 1.f);
        renderer.ui_rect(bx, syb, bw * sf, bh - 4, 0.85f, 0.75f, 0.2f, 0.9f);
        renderer.ui_text(bx, syb - 18, 1.2f, "STAMINA", 0.7f, 0.72f, 0.75f, 0.9f);

        // Top-left: faction + kit.
        const auto& fac_hud = dalian::faction_at(player_faction_id);
        const char* fac = fac_hud.country;
        std::snprintf(buf, sizeof(buf), "%s  -  %s", fac, kits[player_kit].name);
        const std::string fac_line = dalian::truncate_text(renderer, buf, 1.7f, W * 0.45f);
        const float fac_w = renderer.ui_text_width(fac_line.c_str(), 1.7f);
        renderer.ui_rect(20, 20, fac_w + 20, 30, 0.05f, 0.06f, 0.08f, 0.55f);
        renderer.ui_text(30, 27, 1.7f, fac_line.c_str(), 0.65f, 0.82f, 1.0f, 1.f);
        // Multiplayer status badge under the faction line.
        if (net.active()) {
          char nb[96];
          const int in_match = 1 + net.peer_count();
          if (net.is_server())
            std::snprintf(nb, sizeof(nb), "MP HOST  |  %d in match", in_match);
          else if (net.connected())
            std::snprintf(nb, sizeof(nb), "MP CLIENT  |  id %u  |  %d in match", net.local_id(),
                          in_match);
          else
            std::snprintf(nb, sizeof(nb), "MP CONNECTING...");
          dalian::draw_clipped_text(renderer, 30, 56, 280.f, 1.3f, nb, 0.55f, 0.95f, 0.75f, 1.f);
          float ny = 78.f;
          const float mp_list_max_y = 108.f;
          const auto& members = net.lobby().members;
          const std::size_t max_show = 3;
          for (std::size_t mi = 0; mi < members.size() && mi < max_show; ++mi) {
            if (ny > mp_list_max_y) break;
            const auto& m = members[mi];
            char pline[80];
            std::snprintf(pline, sizeof(pline), "%s%s", m.is_host ? "[H] " : "    ", m.name.c_str());
            dalian::draw_clipped_text(renderer, 30, ny, 260.f, 1.1f, pline, 0.75f, 0.78f, 0.82f,
                                      0.95f);
            ny += 18.f;
          }
          if (members.size() > max_show) {
            char more[32];
            std::snprintf(more, sizeof(more), "+%zu more...", members.size() - max_show);
            dalian::draw_clipped_text(renderer, 30, ny, 260.f, 1.0f, more, 0.6f, 0.63f, 0.66f,
                                      0.9f);
          }
        }

        // Bottom-right: weapon + ammo.
        const char* wn = weapon_defs[weapon_index].name;
        if (reloading)
          std::snprintf(buf, sizeof(buf), "%s   RELOADING...", wn);
        else
          std::snprintf(buf, sizeof(buf), "%s   %d / %d", wn, mag_ammo, reserve_ammo);
        const float aw = renderer.ui_text_width(buf, 2.0f);
        renderer.ui_text(W - aw - 30, H - 44, 2.0f, buf, mag_ammo == 0 ? 0.95f : 0.92f,
                         mag_ammo == 0 ? 0.3f : 0.94f, mag_ammo == 0 ? 0.3f : 0.6f, 1.f);
        // Gadgets line above the ammo.
        std::snprintf(buf, sizeof(buf), "GRENADE x%d    C4 x%d%s", grenades_left, c4_left,
                      has_medkit ? "    MEDKIT (H)" : (has_at ? "    AT (RMB)" : ""));
        renderer.ui_text(W - renderer.ui_text_width(buf, 1.4f) - 30, H - 66, 1.4f, buf, 0.8f, 0.82f,
                         0.85f, 1.f);

        // "[E] Enter" / "[F8] Car SAM" only when standing next to a ground vehicle.
        int nearest = -1;
        float nd2 = 6.f * 6.f;
        for (std::size_t i = 0; i < vehicles.size(); ++i) {
          if (vehicles[i].mesh_key.find("vehicles/") == std::string::npos) continue;
          const float dx = vehicles[i].pos.x - player.position.x;
          const float dz = vehicles[i].pos.z - player.position.z;
          const float d2 = dx * dx + dz * dz;
          if (d2 < nd2) {
            nd2 = d2;
            nearest = static_cast<int>(i);
          }
        }
        if (nearest >= 0) {
          std::string mk = vehicles[nearest].mesh_key;
          const auto sl = mk.rfind('/');
          std::string base = sl == std::string::npos ? mk : mk.substr(sl + 1);
          const auto dot = base.find('.');
          if (dot != std::string::npos) base = base.substr(0, dot);
          for (auto& ch : base)
            ch = ch == '_' ? ' ' : static_cast<char>(std::toupper((unsigned char)ch));
          char p[96];
          std::snprintf(p, sizeof(p), "[E]  ENTER  %s", base.c_str());
          const float tw = renderer.ui_text_width(p, 1.8f);
          renderer.ui_rect(W * 0.5f - tw * 0.5f - 14, H * 0.6f - 7, tw + 28, 34, 0.05f, 0.06f, 0.08f,
                           0.6f);
          renderer.ui_text(W * 0.5f - tw * 0.5f, H * 0.6f, 1.8f, p, 0.95f, 0.9f, 0.5f, 1.f);
          if (!vehicles[nearest].is_air) {
            const char* sam_hint = "[F8]  CAR SAM  —  open map, click destination";
            const float stw = renderer.ui_text_width(sam_hint, 1.35f);
            renderer.ui_text(W * 0.5f - stw * 0.5f, H * 0.6f + 36.f, 1.35f, sam_hint, 0.45f, 0.85f,
                             1.f, 1.f);
          }
        } else if (hawk_present) {
          const float dx = hawk_pos.x - player.position.x;
          const float dz = hawk_pos.z - player.position.z;
          if (dx * dx + dz * dz < 14.f * 14.f) {
            char p[96];
            std::snprintf(p, sizeof(p), "[E]  USE  MIM-23 HAWK   (%d left)", hawk_ammo);
            const float tw = renderer.ui_text_width(p, 1.8f);
            renderer.ui_rect(W * 0.5f - tw * 0.5f - 14, H * 0.6f - 7, tw + 28, 34, 0.05f, 0.06f,
                             0.08f, 0.6f);
            renderer.ui_text(W * 0.5f - tw * 0.5f, H * 0.6f, 1.8f, p, 0.95f, 0.85f, 0.45f, 1.f);
            const char* hint = "[F8]  open map, click destination — artillery SAM";
            const float stw = renderer.ui_text_width(hint, 1.35f);
            renderer.ui_text(W * 0.5f - stw * 0.5f, H * 0.6f + 36.f, 1.35f, hint, 0.45f, 0.85f, 1.f,
                             1.f);
          }
        }
        if (sam_hud_flash > 0.f) {
          const char* away = "SAM LAUNCH";
          const float a = std::clamp(sam_hud_flash, 0.f, 1.f);
          const float tw = renderer.ui_text_width(away, 2.2f);
          renderer.ui_text(W * 0.5f - tw * 0.5f, H * 0.42f, 2.2f, away, 1.f, 0.55f, 0.15f, a);
        }
      } else if (in_vehicle >= 0 && vehicles[in_vehicle].is_air) {
        // Helicopter / jet cockpit HUD with radar altimeter and quaternion attitude.
        const Vehicle& v = vehicles[in_vehicle];
        const glm::quat q_yaw = glm::angleAxis(glm::radians(v.heading), glm::vec3(0, 1, 0));
        const glm::quat q_pitch = glm::angleAxis(glm::radians(-v.pitch), glm::vec3(1, 0, 0));
        const glm::quat q_roll = glm::angleAxis(glm::radians(-v.roll), glm::vec3(0, 0, 1));
        const glm::quat orient = q_yaw * q_pitch * q_roll;
        const glm::vec3 nose = orient * glm::vec3(0.f, 0.f, 1.f);
        const glm::vec3 body_up = orient * glm::vec3(0.f, 1.f, 0.f);
        const float pitch_hud = glm::degrees(std::asin(glm::clamp(nose.y, -1.f, 1.f)));
        const float bank_hud = glm::degrees(std::atan2(-body_up.x, body_up.y));

        const auto alt_ray =
            world.raycast({v.pos.x, v.pos.y + 2.f, v.pos.z}, {0.f, -1.f, 0.f}, 6000.f);
        const float radar_alt =
            alt_ray.hit ? std::max(0.f, alt_ray.distance - (v.is_heli ? 1.5f : v.land_clearance + 0.5f))
                        : v.pos.y;
        const float spd_kmh = glm::length(v.vel) * 3.6f;

        char buf[96];
        if (v.is_heli) {
          std::snprintf(buf, sizeof(buf), "ALT %.0f m    SPD %.0f km/h    RPM %.0f%%", radar_alt,
                        spd_kmh, v.rotor_rpm * 100.f);
          renderer.ui_text(30, H - 46, 1.8f, buf, 0.7f, 0.9f, 0.75f, 1.f);
        } else {
          const bool ab_on = jet_ab_lit;
          const char* gear =
              v.jet_gear_anim > 0.95f ? "UP" : (v.jet_gear_anim < 0.05f ? "DN" : "...");
          // THR = commanded, ENG = spool. AB% only while lit (was sprint fuel always — looked wrong).
          if (ab_on) {
            std::snprintf(buf, sizeof(buf),
                          "ALT %.0f m    SPD %.0f km/h    THR %.0f%%    ENG %.0f%%    AB %.0f%%    GEAR %s",
                          radar_alt, spd_kmh, v.throttle * 100.f, v.jet_rpm * 100.f,
                          v.jet_sprint * 100.f, gear);
          } else {
            std::snprintf(buf, sizeof(buf),
                          "ALT %.0f m    SPD %.0f km/h    THR %.0f%%    ENG %.0f%%    AB OFF    GEAR %s",
                          radar_alt, spd_kmh, v.throttle * 100.f, v.jet_rpm * 100.f, gear);
          }
          renderer.ui_text(30, H - 46, 1.8f, buf, ab_on ? 1.f : 0.7f, ab_on ? 0.55f : 0.9f,
                           ab_on ? 0.25f : 0.75f, 1.f);
        }
        std::snprintf(buf, sizeof(buf), "PITCH %+.0f   BANK %+.0f", pitch_hud, bank_hud);
        renderer.ui_text(30, H - 70, 1.4f, buf, 0.6f, 0.8f, 0.85f, 1.f);
        // Weapons block, bottom-right.
        const bool rk = heli_rocket_cd <= 0.f;
        std::snprintf(buf, sizeof(buf), "ROCKETS %s", rk ? "READY" : "...");
        renderer.ui_text(W - renderer.ui_text_width(buf, 1.8f) - 30, H - 46, 1.8f, buf,
                         rk ? 0.4f : 0.8f, rk ? 0.95f : 0.5f, 0.4f, 1.f);
        const bool fl = heli_flare_cd <= 0.f;
        std::snprintf(buf, sizeof(buf), "[X] FLARES %s", fl ? "READY" : "RELOADING");
        renderer.ui_text(W - renderer.ui_text_width(buf, 1.4f) - 30, H - 70, 1.4f, buf,
                         fl ? 0.9f : 0.6f, fl ? 0.85f : 0.6f, 0.4f, 1.f);
        if (v.seats.size() > 1) {
          // Honest gunner read-out: who mans it and whether it's shooting.
          const bool player_gun = player_seat == 1;
          const char* gt;
          float gr = 0.75f, gg = 0.8f, gb = 0.85f;
          if (player_gun) {
            gt = "GUNNER: YOU  (LMB gun, RMB rocket)";
            gr = 0.6f; gg = 0.95f; gb = 0.7f;
          } else if (gunner_engaging) {
            gt = "GUNNER: ENGAGING";
            gr = 1.0f; gg = 0.5f; gb = 0.35f;
          } else if (gunner_target >= 0) {
            gt = "GUNNER: TRACKING";
            gr = 0.95f; gg = 0.85f; gb = 0.4f;
          } else {
            gt = "GUNNER: SCANNING";
          }
          renderer.ui_text(W - renderer.ui_text_width(gt, 1.3f) - 30, H - 90, 1.3f, gt, gr, gg, gb, 1.f);
        }
        // Faction badge, top-left (kept consistent with the on-foot HUD).
        const auto& fac_hud = dalian::faction_at(player_faction_id);
        const char* fac = fac_hud.country;
        std::snprintf(buf, sizeof(buf), "%s  -  %s", fac,
                      v.is_heli ? "ROTARY WING" : "FIXED WING");
        renderer.ui_rect(20, 20, renderer.ui_text_width(buf, 1.7f) + 20, 30, 0.05f, 0.06f, 0.08f,
                         0.55f);
        renderer.ui_text(30, 27, 1.7f, buf, 0.65f, 0.82f, 1.0f, 1.f);
      }

      // ---- Crew seat panel (any vehicle) ------------------------------------
      // Lists every seat with its function key and occupancy so the player can
      // see what's free and jump between stations with F1..Fn (BF2-style).
      if (in_vehicle >= 0 && !vehicles[in_vehicle].seats.empty()) {
        const Vehicle& v = vehicles[in_vehicle];
        const int n = static_cast<int>(v.seats.size());
        const float lh = 22.f;                 // line height
        const float panel_h = 24.f + n * lh;   // header + rows
        const float panel_w = 210.f;
        const float px = static_cast<float>(W) - panel_w - 20.f;
        const float py = 96.f;
        renderer.ui_rect(px, py, panel_w, panel_h, 0.05f, 0.06f, 0.08f, 0.55f);
        renderer.ui_text(px + 12.f, py + 6.f, 1.3f, "CREW  (F1-F8 switch)", 0.7f, 0.82f, 0.95f, 1.f);
        for (int i = 0; i < n; ++i) {
          const int occ = i == player_seat ? 0 : v.seats[i].occupant;
          const char* who = occ == 0 ? "YOU" : occ == 1 ? "AI" : "EMPTY";
          float r, g, b;
          if (occ == 0) { r = 0.55f; g = 0.95f; b = 1.0f; }        // player: cyan
          else if (occ == 1) { r = 0.95f; g = 0.8f; b = 0.4f; }    // AI: amber
          else { r = 0.55f; g = 0.6f; b = 0.62f; }                 // empty: grey
          char line[64];
          std::snprintf(line, sizeof(line), "F%d  %-9s %s", i + 1, v.seats[i].name, who);
          renderer.ui_text(px + 12.f, py + 26.f + i * lh, 1.25f, line, r, g, b, 1.f);
        }
      }

      // ---- Scoreboard (hold Tab — BF2 default) ------------------------------
      if (scoreboard_open && !deploy_open && !drone_mode) {
        const float sx = W * 0.5f - 280.f;
        const float sy = 80.f;
        renderer.ui_rect(sx, sy, 560.f, 220.f, 0.04f, 0.05f, 0.07f, 0.88f);
        renderer.ui_text(sx + 16.f, sy + 10.f, 1.8f, "SCOREBOARD", 0.95f, 0.96f, 0.98f, 1.f);
        char line[128];
        std::snprintf(line, sizeof(line), "You   K %d   D %d   HP %.0f", player_kills, player_deaths,
                      player_health);
        renderer.ui_text(sx + 16.f, sy + 44.f, 1.35f, line, 0.75f, 0.82f, 0.95f, 1.f);
        std::snprintf(line, sizeof(line), "Tickets   %d  —  %d", tickets.team1_tickets,
                      tickets.team2_tickets);
        renderer.ui_text(sx + 16.f, sy + 72.f, 1.25f, line, 0.7f, 0.73f, 0.76f, 1.f);
        renderer.ui_text(sx + 16.f, sy + 100.f, 1.1f,
                         "Commo rose, chat, squad screen — coming soon (BF2 keys reserved).", 0.55f,
                         0.58f, 0.62f, 1.f);
      }

      // ---- Conquest HUD: ticket bleed + tactical minimap --------------------
      if (!deploy_open && !drone_mode && !conquest_points.empty()) {
        auto team_rgb = [](dalian::TeamId t, float& r, float& g, float& b) {
          if (t == dalian::TeamId::Team1) {
            r = 0.55f;
            g = 0.75f;
            b = 1.0f;
          } else if (t == dalian::TeamId::Team2) {
            r = 0.95f;
            g = 0.35f;
            b = 0.3f;
          } else {
            r = 0.5f;
            g = 0.52f;
            b = 0.55f;
          }
        };

        const auto& fac_friendly = dalian::faction_at(player_faction_id);
        const auto& fac_enemy = dalian::faction_at(enemy_faction_id);
        const bool player_is_team1 = player_team == dalian::TeamId::Team1;
        const int friendly_tickets =
            player_is_team1 ? tickets.team1_tickets : tickets.team2_tickets;
        const int enemy_tickets = player_is_team1 ? tickets.team2_tickets : tickets.team1_tickets;
        const float bar_w = 420.f;
        const float bar_x = W * 0.5f - bar_w * 0.5f;
        const float bar_y = H - 52.f;
        renderer.ui_rect(bar_x, bar_y, bar_w, 36.f, 0.05f, 0.06f, 0.08f, 0.72f);
        renderer.ui_text(bar_x + 16.f, bar_y + 8.f, 2.4f, std::to_string(friendly_tickets).c_str(),
                         0.65f, 0.82f, 1.0f, 1.f);
        const float t2w = renderer.ui_text_width(std::to_string(enemy_tickets).c_str(), 2.4f);
        renderer.ui_text(bar_x + bar_w - t2w - 16.f, bar_y + 8.f, 2.4f,
                         std::to_string(enemy_tickets).c_str(), 0.95f, 0.4f, 0.35f, 1.f);
        dalian::draw_clipped_text(renderer, bar_x + 72.f, bar_y + 10.f, bar_w * 0.5f - 96.f, 1.0f,
                                  fac_friendly.country, 0.55f, 0.7f, 0.95f, 0.85f);
        dalian::draw_clipped_text(renderer, bar_x + bar_w * 0.5f + 24.f, bar_y + 10.f,
                                  bar_w * 0.5f - 96.f, 1.0f, fac_enemy.country, 0.9f, 0.45f, 0.4f,
                                  0.85f);

        const float mmx = 24.f;
        const float mmy = 118.f;
        const float mmw = 200.f;
        const float mmh = 200.f;
        renderer.ui_rect(mmx, mmy, mmw, mmh, 0.06f, 0.08f, 0.1f, 0.82f);
        renderer.ui_rect(mmx, mmy, mmw, 2.f, 0.35f, 0.5f, 0.6f, 1.f);
        glm::vec2 mn(1e30f), mx(-1e30f);
        for (const auto& cp : conquest_points) {
          mn.x = std::min(mn.x, cp.pos.x);
          mn.y = std::min(mn.y, cp.pos.z);
          mx.x = std::max(mx.x, cp.pos.x);
          mx.y = std::max(mx.y, cp.pos.z);
        }
        mn.x = std::min(mn.x, player.position.x);
        mn.y = std::min(mn.y, player.position.z);
        mx.x = std::max(mx.x, player.position.x);
        mx.y = std::max(mx.y, player.position.z);
        glm::vec2 span = mx - mn;
        if (span.x < 80.f) span.x = 80.f;
        if (span.y < 80.f) span.y = 80.f;
        const float pad = 18.f;
        dalian::MinimapProjector minimap;
        minimap.configure(mn, mx, mmx + pad, mmy + pad, mmw - 2.f * pad, mmh - 2.f * pad);
        for (const auto& cp : conquest_points) {
          const glm::vec2 c = minimap.world_to_minimap(cp.pos);
          float cr, cg, cb;
          team_rgb(cp.owner, cr, cg, cb);
          const float sz = 10.f;
          renderer.ui_rect(c.x - sz * 0.5f, c.y - sz * 0.5f, sz, sz, cr * 0.35f, cg * 0.35f, cb * 0.35f,
                           0.95f);
          renderer.ui_rect(c.x - sz * 0.5f, c.y - sz * 0.5f, sz * cp.capture_progress, sz, cr, cg, cb,
                           0.95f);
          if (cp.capturer != cp.owner && cp.capture_progress > 0.01f) {
            float ar, ag, ab;
            team_rgb(cp.capturer, ar, ag, ab);
            renderer.ui_rect(c.x - sz * 0.5f, c.y - sz * 0.5f, sz, 2.f, ar, ag, ab, 1.f);
          }
        }
        const glm::vec2 pmm = minimap.world_to_minimap(
            {player.position.x, player.position.y - player.eye_height, player.position.z});
        renderer.ui_rect(pmm.x - 4.f, pmm.y - 4.f, 8.f, 8.f, 0.95f, 0.95f, 0.95f, 1.f);
        if (hawk_present && hawk_ammo > 0) {
          const glm::vec2 hmm = minimap.world_to_minimap(hawk_pos);
          renderer.ui_rect(hmm.x - 5.f, hmm.y - 5.f, 10.f, 10.f, 0.95f, 0.55f, 0.15f, 1.f);
        }
        if (session_mp.enabled && match_started && round_time < 90.f) {
          dalian::draw_clipped_text(renderer, mmx, mmy + mmh + 4.f, mmw + 180.f, 1.0f,
                                    "Ticket bleed paused — waiting for all players (90s grace)",
                                    0.55f, 0.72f, 0.55f, 0.9f);
        }
        const int mins = static_cast<int>(round_time) / 60;
        const int secs = static_cast<int>(round_time) % 60;
        char rbuf[32];
        std::snprintf(rbuf, sizeof(rbuf), "%d:%02d", mins, secs);
        renderer.ui_text(mmx + 8.f, mmy + mmh + 22.f, 1.2f, rbuf, 0.75f, 0.78f, 0.82f, 0.9f);

        for (const auto& cp : conquest_points) {
          if (dalian::xz_distance_sq({player.position.x, player.position.y - player.eye_height,
                              player.position.z},
                             cp.pos) > cp.radius * cp.radius)
            continue;
          if (cp.capturer == player_team && cp.owner != player_team && cp.capture_progress > 0.01f) {
            const int pct = static_cast<int>(cp.capture_progress * 100.f);
            char cap[48];
            std::snprintf(cap, sizeof(cap), "CAPTURING %s  %d%%", cp.name.c_str(), pct);
            const std::string cap_line = dalian::truncate_text(renderer, cap, 1.5f, W - 80.f);
            const float cap_w = renderer.ui_text_width(cap_line.c_str(), 1.5f);
            renderer.ui_rect(W * 0.5f - cap_w * 0.5f - 12.f, H * 0.72f - 8.f, cap_w + 24.f, 28.f,
                             0.05f, 0.08f, 0.12f, 0.78f);
            renderer.ui_text(W * 0.5f - cap_w * 0.5f, H * 0.72f, 1.5f, cap_line.c_str(), 0.55f,
                             0.85f, 1.0f, 1.f);
            break;
          }
        }
      }

      if (match_over && match_started && !deploy_open) {
        const bool player_won = winning_team == player_team;
        const char* headline = player_won ? "VICTORY" : "DEFEAT";
        float hr, hg, hb;
        if (player_won) {
          hr = 0.45f;
          hg = 0.85f;
          hb = 1.0f;
        } else {
          hr = 0.95f;
          hg = 0.35f;
          hb = 0.3f;
        }
        renderer.ui_rect(W * 0.5f - 220.f, H * 0.5f - 70.f, 440.f, 140.f, 0.04f, 0.05f, 0.07f,
                         0.88f);
        const float hw = renderer.ui_text_width(headline, 4.2f);
        renderer.ui_text(W * 0.5f - hw * 0.5f, H * 0.5f - 10.f, 4.2f, headline, hr, hg, hb, 1.f);
        char sub[128];
        std::snprintf(sub, sizeof(sub), "Tickets  %d  -  %d", tickets.team1_tickets,
                      tickets.team2_tickets);
        const float sw = renderer.ui_text_width(sub, 1.6f);
        renderer.ui_text(W * 0.5f - sw * 0.5f, H * 0.5f - 44.f, 1.6f, sub, 0.85f, 0.88f, 0.9f, 1.f);
        const float remain = std::max(0.f, 12.f - match_restart_cd);
        char restart[96];
        std::snprintf(restart, sizeof(restart), "New round in %.0fs...", remain);
        const float rw = renderer.ui_text_width(restart, 1.4f);
        renderer.ui_text(W * 0.5f - rw * 0.5f, H * 0.5f + 18.f, 1.4f, restart, 0.65f, 0.68f, 0.72f,
                         1.f);
      }
      renderer.end_ui();
    }

    // Headless screenshot: capture the composed backbuffer, write PNG, quit.
    if (!shot_path.empty() && ++frame_no >= shot_frames) {
      std::vector<unsigned char> px(static_cast<std::size_t>(cur_w) * cur_h * 3);
      glReadPixels(0, 0, cur_w, cur_h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
      std::vector<unsigned char> flip(px.size());
      const int row = cur_w * 3;
      for (int y = 0; y < cur_h; ++y)
        std::memcpy(&flip[static_cast<std::size_t>(y) * row],
                    &px[static_cast<std::size_t>(cur_h - 1 - y) * row], row);
      stbi_write_png(shot_path.c_str(), cur_w, cur_h, 3, flip.data(), row);
      std::cout << "Wrote screenshot " << shot_path << " (" << cur_w << "x" << cur_h << ")\n";
      running = false;
    }

    SDL_GL_SwapWindow(window);

    if (now - title_timer > SDL_GetPerformanceFrequency() / 4) {
      title_timer = now;
      char title[320];
      if (drone_mode) {
        std::snprintf(title, sizeof(title),
                      "Project Dalian  |  %s  |  batt %.0f%%  thr %.0f%%  sig %.0f%%  |  %.0f fps  |"
                      "  %s  ESC",
                      kamikaze_mode ? "FPV KAMIKAZE" : "FPV RECON",
                      drone.battery * 100.f, drone_throttle * 100.f, signal * 100.f,
                      dt > 0 ? 1.f / dt : 0.f,
                      kamikaze_mode ? "LMB/Space detonate, W/S throttle, mouse steer, no recall"
                                    : "mouse pitch/roll, A/D yaw, W/S throttle, F9 recall");
      } else if (in_vehicle >= 0) {
        const Vehicle& v = vehicles[in_vehicle];
        float drive_readout = v.is_air ? v.pos.y : v.speed * 3.6f;
        const char* drive_unit = v.is_air ? "m alt" : "km/h";
        if (v.is_air && !v.is_heli && v.wheels_on_ground) {
          drive_readout = v.jet_rpm * 100.f;
          drive_unit = "% ENG";
        }
        std::snprintf(title, sizeof(title),
                      "Project Dalian  |  DRIVING %s  |  %.0f %s  |  HP %.0f  |  %.0f fps  |  %s",
                      v.is_air ? "AIRCRAFT" : "GROUND", drive_readout, drive_unit, player_health,
                      dt > 0 ? 1.f / dt : 0.f,
                      (v.is_air && v.is_heli)
                          ? "W up / S down, A/D yaw, mouse cyclic, Shift boost, LMB rockets, "
                            "X flares, F1-F2 seats, E exit"
                      : v.is_air
                          ? (v.wheels_on_ground
                                 ? "W spool, hold W/double-tap W/Shift/Ctrl AB, G gear, A/D "
                                   "rudder, pull BACK after ~72 km/h to rotate, E exit"
                                 : "W/S throttle, hold W/double-tap W/Shift/Ctrl AB, mouse "
                                   "pitch/roll, G gear, A/D rudder, E exit")
                          : "W/S drive, A/D steer, Shift boost, F1-F3 seats, E exit");
      } else {
        std::snprintf(title, sizeof(title),
                      "Project Dalian  |  %s  |  %s  |  HP %.0f  |  K %d  D %d  |  %.0f fps  |  "
                      "LMB fire, R reload, 4 nade, 5 C4, X detonate, F8 SAM map, F9/F10 drones, "
                      "E enter, ENTER deploy, C 1st/3rd, ESC pause",
                      kits[player_kit].name, have_weapon_model ? weapon_defs[weapon_index].name : "gun",
                      player_health, player_kills, player_deaths, dt > 0 ? 1.f / dt : 0.f);
      }
      SDL_SetWindowTitle(window, title);
    }
  }

  renderer.destroy_color(gun_mesh);
  for (auto& wm : weapon_meshes) {
    renderer.destroy_textured(wm);
  }
  renderer.destroy_skinned(soldier_mesh);
  renderer.destroy_skinned(enemy_mesh);
  for (auto& [path, mesh] : template_cache) {
    renderer.destroy_textured(mesh);
  }
  for (auto& [path, mesh] : vehicle_cache) {
    renderer.destroy_textured(mesh);
  }
  if (have_ground_tex) {
    renderer.destroy_textured(terrain_tex_gpu);
    bf2::TerrainColormapLoader::destroy(ground_tex);
  } else {
    renderer.destroy_mesh(terrain_gpu);
  }
  textures.clear();

  if (advertise_session) session_discovery.stop();
  if (have_game_audio) game_audio.shutdown();

    if (leave_to_menu) {
      continue;
    }
    break;
  }  // app_running

  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
