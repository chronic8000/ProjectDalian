// Project Dalian: a modern tactical war-sim built on real BF2 (Refractor 2)
// assets. Successor to the "Dalian Plant" map that gave the project its name.
//
// Mounts a level's server.zip (+ the mod's Objects_client.zip), builds the
// terrain, resolves static-object placements to textured meshes, and drops you
// in as a soldier with animated enemies, ballistics, an FPV drone and shadows.
//
//   project_dalian <level_server.zip> [objects_client.zip]
//
// Controls: WASD move, mouse look, Shift sprint, Space jump, V 1st/3rd,
// LMB fire, wheel/Q swap weapon, F ballistic/hitscan, B launch/recall drone,
// ESC quit.
#include <GL/glew.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <utility>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/anim/pose.hpp"
#include "engine/anim/skinning.hpp"
#include "engine/core/atmosphere.hpp"
#include "engine/core/level_loader.hpp"
#include "engine/core/object_lightmaps.hpp"
#include "engine/core/undergrowth.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"
#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/collision/bf2_collision.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/formats/terrain/terrain_colormap.hpp"
#include "engine/physics/drone.hpp"
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
  const float off_x = (w * 0.5f) * xz_scale;
  const float off_z = (h * 0.5f) * xz_scale;

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
glm::mat4 placement_matrix(const bf2::ObjectInstance& inst) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f),
                               glm::vec3(inst.position[0], inst.position[1], inst.position[2]));
  m = glm::rotate(m, glm::radians(inst.rotation[0]), glm::vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(inst.rotation[1]), glm::vec3(1, 0, 0));
  m = glm::rotate(m, glm::radians(inst.rotation[2]), glm::vec3(0, 0, 1));
  return m;
}

struct Instance {
  std::string mesh_key;  // stable lookup into template_cache (never store raw map pointers)
  glm::mat4 model{1.0f};
  glm::vec3 origin{};
  std::uint32_t lightmap = 0;         // baked object lightmap atlas texture (0 = none)
  glm::vec4 lm_xform{1.f, 1.f, 0.f, 0.f};
};

// Extract a local-space triangle soup (3 verts per tri) from the collision mesh
// col best suited to soldier movement (col_type 2, else the densest col).
std::vector<bf2::Float3> collision_soup(const bf2::CollisionMesh& cm) {
  const bf2::CollisionCol* best = nullptr;
  for (const auto& col : cm.cols) {
    if (col.col_type == 2) {
      best = &col;
      break;
    }
  }
  if (best == nullptr) {
    for (const auto& col : cm.cols) {
      if (best == nullptr || col.faces.size() > best->faces.size()) {
        best = &col;
      }
    }
  }
  std::vector<bf2::Float3> out;
  if (best == nullptr) {
    return out;
  }
  const std::size_t vcount = best->vertices.size();
  out.reserve(best->faces.size() * 3);
  for (const auto& f : best->faces) {
    if (f.v1 < vcount && f.v2 < vcount && f.v3 < vcount) {
      out.push_back(best->vertices[f.v1]);
      out.push_back(best->vertices[f.v2]);
      out.push_back(best->vertices[f.v3]);
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
  const std::string suffix = ".staticmesh";
  if (static_mesh_vpath.size() > suffix.size() &&
      static_mesh_vpath.compare(static_mesh_vpath.size() - suffix.size(), suffix.size(), suffix) ==
          0) {
    return static_mesh_vpath.substr(0, static_mesh_vpath.size() - suffix.size()) + ".collisionmesh";
  }
  return {};
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
  if (argc < 2) {
    std::cerr << "Usage: project_dalian <level_server.zip> [objects_client.zip]\n";
    return 1;
  }
  const std::string archive_path = argv[1];

  // Optional headless screenshot: --shot <path.png> [frames] [drone]. Renders a
  // few frames (so terrain/objects/shadows settle) then writes a PNG and exits.
  std::string shot_path;
  int shot_frames = 30;
  bool shot_drone = false;
  bool shot_third_person = false;
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
    }
  }

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
    return 1;
  }
  const bool have_objects = !objects_zip.empty() && std::filesystem::exists(objects_zip) &&
                            resources.archives().mount(objects_zip);
  std::cout << "Objects archive: " << (have_objects ? objects_zip : "(none)") << '\n';

  // Common_client.zip holds the shared terrain detail textures used by the
  // per-patch splat (Terrain/Textures/Detail/...).
  if (!objects_zip.empty()) {
    const auto common_zip =
        (std::filesystem::path(objects_zip).parent_path() / "Common_client.zip").string();
    if (std::filesystem::exists(common_zip) && resources.archives().mount(common_zip)) {
      std::cout << "Common archive: " << common_zip << '\n';
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
    return 1;
  }

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
  const bf2::Atmosphere atmo = bf2::parse_atmosphere(water_con, sky_con, heightdata_con);
  std::cout << "Atmosphere: sky " << atmo.sky_color.x << "/" << atmo.sky_color.y << "/"
            << atmo.sky_color.z << (atmo.has_water ? "  water level " : "  (no water")
            << (atmo.has_water ? std::to_string(atmo.water_level) : std::string(")")) << '\n';

  bf2::TemplateResolver resolver(resources);
  if (const auto so = resources.read_bytes("StaticObjects.con")) {
    const std::string script(reinterpret_cast<const char*>(so->data()), so->size());
    resolver.build_from_static_objects(script);
  }
  std::cout << "Resolved " << resolver.map().size() << " templates; " << level.placements.size()
            << " placements\n";

  // Window + GL.
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
  // 4x MSAA for smoother edges.
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

  const int win_w = 1600;
  const int win_h = 900;
  SDL_Window* window = SDL_CreateWindow("Project Dalian", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        win_w, win_h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
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
  SDL_GL_SetSwapInterval(1);

  bf2::Renderer renderer;
  renderer.initialize(window);
  bf2::TextureCache textures(resources, renderer);

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
        grass_atlas_tex = renderer.upload_texture(resources.load_texture("UndergrowthAtlas0.dds"));
      } catch (const std::exception&) {
      }
      std::cout << "Undergrowth: " << undergrowth.width << "x" << undergrowth.height << " map, "
                << undergrowth.grass.size() << " grass materials, cell " << cell << "m\n";
    }
  }

  // Build one textured GPU mesh per unique resolved template.
  std::unordered_map<std::string, bf2::GpuTexturedMesh> template_cache;
  template_cache.reserve(512);
  // Local-space collision triangle soup per unique template.
  std::unordered_map<std::string, std::vector<bf2::Float3>> collision_cache;
  collision_cache.reserve(512);
  std::vector<Instance> instances;
  instances.reserve(level.placements.size());
  int resolved = 0;
  for (const auto& inst : level.placements) {
    const std::string vpath = resolver.resolve_mesh(inst.template_name);
    if (vpath.empty()) {
      continue;
    }
    auto it = template_cache.find(vpath);
    if (it == template_cache.end()) {
      bf2::GpuTexturedMesh gpu{};
      try {
        const auto m = resources.load_mesh(vpath);
        const auto data = bf2::MeshLoader::extract_textured(m, 0, 0);  // highest detail
        if (!data.vertices.empty()) {
          gpu = renderer.upload_textured(data);
          for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
            gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
            gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
            gpu.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map);
            gpu.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map);
            gpu.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map);
          }
        }
      } catch (const std::exception&) {
      }
      it = template_cache.emplace(vpath, std::move(gpu)).first;

      // Load matching collision mesh once per template.
      std::vector<bf2::Float3> soup;
      const std::string col_vpath = collision_path_for(vpath);
      if (!col_vpath.empty()) {
        if (const auto bytes = resources.read_bytes(col_vpath)) {
          try {
            const auto cm = bf2::CollisionLoader::load_from_memory(*bytes);
            soup = collision_soup(cm);
          } catch (const std::exception&) {
          }
        }
      }
      collision_cache.emplace(vpath, std::move(soup));
    }
    if (it->second.vao != 0) {
      Instance in;
      in.mesh_key = vpath;
      in.model = placement_matrix(inst);
      in.origin = glm::vec3(inst.position[0], inst.position[1], inst.position[2]);
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
    }
  }
  std::cout << "Uploaded " << template_cache.size() << " unique meshes, " << resolved
            << " instances; textures loaded " << textures.loaded_count() << ", missing "
            << textures.missing_count() << '\n';

  // Physics / player.
  bf2::PhysicsWorld world;
  world.set_terrain(level.terrain, xz, /*centered=*/true);

  // Feed world-space collision triangles for every placed building.
  for (const auto& in : instances) {
    const auto cit = collision_cache.find(in.mesh_key);
    if (cit == collision_cache.end()) {
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

  bf2::CharacterController player;
  player.eye_height = 1.8f;

  // Spawn at the original BF2 spawn: the control point nearest the map centre
  // that sits above the water line. Falls back to map centre when no gameplay
  // layout is found.
  glm::vec3 spawn(0.f, world.terrain_height(0.f, 0.f) + player.eye_height + 2.f, 0.f);
  std::string gameplay_script;
  std::vector<glm::vec3> control_points;  // above-water objectives, for enemy placement
  {
    std::vector<glm::vec3> cps;
    for (const char* gm : {"GameModes/gpm_coop/16/GamePlayObjects.con",
                           "GameModes/gpm_cq/64/GamePlayObjects.con",
                           "GameModes/gpm_cq/32/GamePlayObjects.con",
                           "GameModes/gpm_cq/16/GamePlayObjects.con",
                           "GameModes/sp1/16/GamePlayObjects.con"}) {
      if (const auto b = resources.read_bytes(gm)) {
        std::string script(reinterpret_cast<const char*>(b->data()), b->size());
        cps = parse_control_points(script);
        if (!cps.empty()) {
          gameplay_script = std::move(script);
          std::cout << "Spawns: " << cps.size() << " control points from " << gm << '\n';
          break;
        }
      }
    }
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
  }
  player.position = {spawn.x, spawn.y, spawn.z};
  std::cout << "Spawn @ " << spawn.x << " " << spawn.y << " " << spawn.z << '\n';

  // Vehicles: placed by ObjectSpawners in the gameplay layout. Each vehicle is a
  // bundledmesh; its 3rd-person model is the geometry with the most triangles
  // (geom0 is the minimal 1st-person view). Rendered as static props at bind pose.
  struct Vehicle {
    std::string mesh_key;
    glm::mat4 model{1.0f};
    glm::vec3 origin{};
  };
  std::vector<Vehicle> vehicles;
  std::unordered_map<std::string, bf2::GpuTexturedMesh> vehicle_cache;
  if (!gameplay_script.empty()) {
    // Index every .bundledmesh by base name so vehicle templates resolve to a mesh.
    std::unordered_map<std::string, std::string> bundled_by_name;
    for (const auto& p : resources.archives().list()) {
      if (p.size() > 12 && p.compare(p.size() - 12, 12, ".bundledmesh") == 0) {
        bundled_by_name.emplace(bf2::detail::basename_no_ext(p), p);
      }
    }
    auto best_geometry = [](const bf2::Mesh& mesh) {
      std::size_t best = 0;
      std::uint32_t best_n = 0;
      for (std::size_t g = 0; g < mesh.geometries.size(); ++g) {
        if (mesh.geometries[g].lods.empty()) continue;
        std::uint32_t n = 0;
        for (const auto& m : mesh.geometries[g].lods[0].materials) n += m.index_count;
        if (n > best_n) {
          best_n = n;
          best = g;
        }
      }
      return best;
    };
    const auto spawns = parse_vehicle_spawns(gameplay_script);
    int placed = 0;
    for (const auto& vs : spawns) {
      const auto mit = bundled_by_name.find(vs.vehicle);
      if (mit == bundled_by_name.end()) continue;
      const std::string& vpath = mit->second;
      if (vehicle_cache.find(vpath) == vehicle_cache.end()) {
        bf2::GpuTexturedMesh gpu;
        try {
          const auto mesh = resources.load_mesh(vpath);
          const auto data = bf2::MeshLoader::extract_textured(mesh, best_geometry(mesh), 0);
          if (!data.vertices.empty()) {
            gpu = renderer.upload_textured(data);
            for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
              gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
              gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
              gpu.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map);
              gpu.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map);
              gpu.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map);
            }
          }
        } catch (const std::exception&) {
        }
        vehicle_cache[vpath] = std::move(gpu);
      }
      if (vehicle_cache[vpath].vao == 0) continue;
      Vehicle v;
      v.mesh_key = vpath;
      v.origin = vs.pos;
      glm::mat4 m = glm::translate(glm::mat4(1.0f), vs.pos);
      m = glm::rotate(m, glm::radians(vs.rot.x), glm::vec3(0, 1, 0));
      m = glm::rotate(m, glm::radians(vs.rot.y), glm::vec3(1, 0, 0));
      m = glm::rotate(m, glm::radians(vs.rot.z), glm::vec3(0, 0, 1));
      v.model = m;
      vehicles.push_back(v);
      ++placed;
    }
    std::cout << "Vehicles: " << placed << " placed, " << vehicle_cache.size() << " unique meshes\n";
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

  bf2::GpuColorMesh gun_mesh{};
  if (!have_weapon_model) {
    std::vector<float> gv;
    std::vector<std::uint32_t> gi;
    build_gun_mesh(gv, gi);
    gun_mesh = renderer.upload_color(gv, gi);
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
  bf2::AnimationClip clip_stand, clip_walk, clip_run;
  bool have_soldier = false;
  bool have_enemy_mesh = false;  // true when a distinct opfor model loaded
  bool have_clip_stand = false, have_clip_walk = false, have_clip_run = false;

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
          enemy_src = std::move(esrc);
          have_enemy_mesh = true;
          std::cout << "Enemy body: " << path << '\n';
          break;
        }
      } catch (const std::exception&) {
      }
    }
    auto try_clip = [&](const char* vpath, bf2::AnimationClip& out) -> bool {
      try {
        if (const auto b = resources.read_bytes(vpath)) {
          out = bf2::AnimationLoader::load_from_memory(*b);
          return out.frame_count > 0;
        }
      } catch (const std::exception&) {
      }
      return false;
    };
    if (have_soldier) {
      have_clip_stand = try_clip("soldiers/common/animations/3p/3p_stand.baf", clip_stand);
      have_clip_walk = try_clip("soldiers/common/animations/3p/3p_walkforward.baf", clip_walk);
      have_clip_run = try_clip("soldiers/common/animations/3p/3p_runforward.baf", clip_run);
    }
    std::cout << "Soldier body: " << (have_soldier ? "US light (3p, animated)" : "none")
              << ", tex " << soldier_tex << "; enemy tex " << enemy_tex << '\n';
    std::cout << "Clips: stand=" << (have_clip_stand ? clip_stand.frame_count : -1)
              << " walk=" << (have_clip_walk ? clip_walk.frame_count : -1)
              << " run=" << (have_clip_run ? clip_run.frame_count : -1)
              << " (ske nodes=" << soldier_ske.nodes.size() << ")\n";
  }
  bool third_person = false;
  float anim_time = 0.f;

  // Shooting state.
  struct Tracer {
    glm::vec3 a, b;
    float life;
  };
  struct Impact {
    glm::vec3 p;
    float life;
  };
  // A physical bullet: integrates velocity + gravity and does a segment raycast
  // each frame between its previous and current position (the "BF2 way"), giving
  // real travel time and bullet drop.
  struct Projectile {
    glm::vec3 pos;
    glm::vec3 vel;
    float life;
  };
  std::vector<Tracer> tracers;
  std::vector<Impact> impacts;
  std::vector<Projectile> projectiles;
  float fire_cooldown = 0.f;  // seconds until next shot allowed
  float muzzle_flash = 0.f;   // >0 while flash visible
  float recoil = 0.f;         // 0..1 recoil amount, decays
  bool ballistic = true;      // true = physical projectiles, false = hitscan
  constexpr float kMuzzleSpeed = 340.f;    // m/s
  constexpr float kBulletGravity = -9.81f;  // real gravity for drop
  constexpr float kBulletLife = 3.0f;       // seconds before despawn
  constexpr float kBulletDrag = 0.0011f;    // quadratic air-drag coefficient
  // Wind: a horizontal breeze that both slows and drifts bullets (drag is
  // computed relative to the moving air, so downrange rounds get carried).
  glm::vec3 wind_base(2.6f, 0.f, 1.4f);
  glm::vec3 wind = wind_base;

  // ---- Enemies: soldier targets with reactive engage AI -------------------
  // Enemies use the same skinned soldier body. Hit registration is done against
  // capsule colliders that track the animated skeleton bones (head/torso/limb),
  // updated every frame. Squads defend the map's objectives rather than swarming
  // the spawn: they detect the player by line of sight, take a moment to react,
  // then fire in controlled bursts with distance-based accuracy.
  struct Enemy {
    glm::vec3 pos{};    // feet position
    glm::vec3 home{};   // patrol anchor / respawn origin
    float yaw = 0.f;    // facing (radians)
    float health = 100.f;
    float anim_time = 0.f;
    float alert = 0.f;        // 0..1 awareness, ramps with LOS, decays without
    float burst_cooldown = 0.f;  // time until next burst
    int burst_left = 0;          // rounds remaining in current burst
    float shot_timer = 0.f;      // time until next round within a burst
    float hit_flash = 0.f;
    float death_time = 0.f;
    bool alive = true;
    bool moving = false;
    glm::vec3 patrol_target{};  // current wander waypoint near home
    float patrol_wait = 0.f;    // dwell timer when idling at a waypoint
    std::vector<Capsule> caps;   // world-space bone capsules (refreshed per frame)
  };
  std::vector<Enemy> enemies;
  auto frand = []() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); };
  auto drop = [&](Enemy& en) {  // snap feet to terrain
    en.pos.y = world.terrain_height(en.pos.x, en.pos.z);
  };
  if (have_soldier) {
    // Garrison the objectives: a small squad defends each above-water control
    // point except the one the player spawns on. Enemies are spread out around
    // their post, so you meet a few at a time as you advance across the map.
    const float min_from_spawn = 55.f;  // keep the immediate spawn area safe
    std::vector<glm::vec3> posts;
    for (const auto& cp : control_points) {
      if (glm::distance(glm::vec2(cp.x, cp.z), glm::vec2(spawn.x, spawn.z)) < min_from_spawn) continue;
      posts.push_back(cp);
    }
    if (posts.empty()) {
      // No usable objectives: scatter a handful far out in random directions.
      for (int i = 0; i < 5; ++i) {
        const float ang = frand() * 6.2831853f;
        const float r = 80.f + frand() * 80.f;
        posts.push_back(glm::vec3(spawn.x + std::cos(ang) * r, 0.f, spawn.z + std::sin(ang) * r));
      }
    }
    for (const auto& post : posts) {
      const int squad = 2 + (std::rand() % 2);  // 2-3 defenders per objective
      for (int i = 0; i < squad; ++i) {
        Enemy en;
        const float ang = frand() * 6.2831853f;
        const float r = frand() * 22.f;  // dispersed around the objective
        en.home = glm::vec3(post.x + std::cos(ang) * r, 0.f, post.z + std::sin(ang) * r);
        en.pos = en.home;
        en.patrol_target = en.home;
        drop(en);
        en.yaw = frand() * 6.2831853f;
        en.burst_cooldown = 0.5f + frand();
        enemies.push_back(en);
      }
    }
    std::cout << "Enemies: " << enemies.size() << " defenders across " << posts.size()
              << " objectives\n";
  }
  int player_kills = 0;
  int player_deaths = 0;
  float player_health = 100.f;
  float player_regen_delay = 0.f;

  // Rebuild an enemy's bone-tracked capsules from its current animation pose.
  auto update_capsules = [&](Enemy& en, const bf2::AnimationClip* clip, int frame) {
    const bf2::PosedSkeleton posed = bf2::pose_skeleton(soldier_ske, clip, frame);
    if (posed.world_positions.size() < 48) return;
    const float cy = std::cos(en.yaw), sy = std::sin(en.yaw);
    auto to_world = [&](const glm::vec3& m) {
      // model-space (x right, y up, z fwd) -> yaw-rotated world at feet.
      return glm::vec3(en.pos.x + m.x * cy + m.z * sy, en.pos.y + m.y,
                       en.pos.z - m.x * sy + m.z * cy);
    };
    en.caps.clear();
    for (const auto& bp : kBonePairs) {
      if (bp.a >= static_cast<int>(posed.world_positions.size()) ||
          bp.b >= static_cast<int>(posed.world_positions.size())) continue;
      Capsule c;
      c.a = to_world(posed.world_positions[bp.a]);
      c.b = to_world(posed.world_positions[bp.b]);
      c.r = bp.r;
      c.zone = bp.zone;
      en.caps.push_back(c);
    }
  };

  struct EnemyHit { int idx = -1; int zone = 0; float dist = 1e30f; glm::vec3 point{}; };
  // Nearest capsule hit along ray o + dir*t (dir normalised), within maxd.
  auto shoot_enemies = [&](const glm::vec3& o, const glm::vec3& dir, float maxd) -> EnemyHit {
    EnemyHit best;
    const glm::vec3 q1 = o + dir * maxd;
    for (std::size_t i = 0; i < enemies.size(); ++i) {
      if (!enemies[i].alive) continue;
      for (const auto& c : enemies[i].caps) {
        float s;
        const float d2 = closest_seg_seg_sq(o, q1, c.a, c.b, s);
        if (d2 <= c.r * c.r) {
          const float t = s * maxd;
          if (t < best.dist) {
            best.dist = t;
            best.idx = static_cast<int>(i);
            best.zone = c.zone;
            best.point = o + dir * t;
          }
        }
      }
    }
    return best;
  };
  auto damage_enemy = [&](int idx, int zone) {
    Enemy& en = enemies[idx];
    const float dmg = zone == 2 ? 100.f : (zone == 1 ? 42.f : 20.f);  // head / torso / limb
    en.health -= dmg;
    en.hit_flash = 0.12f;
    en.alert = 1.f;  // being shot instantly alerts
    if (en.health <= 0.f) {
      en.alive = false;
      en.death_time = 0.f;
      ++player_kills;
    }
  };

  float yaw = -90.f;    // looking toward -Z
  float pitch = -5.f;
  const float sensitivity = 0.12f;

  SDL_SetRelativeMouseMode(SDL_TRUE);

  bool running = true;
  bool mouse_look = true;
  Uint64 prev = SDL_GetPerformanceCounter();
  const float draw_dist = 450.f;
  const float draw_dist2 = draw_dist * draw_dist;

  // FPV drone: a 6-DoF quadcopter you can launch and fly. Mouse steers pitch/
  // roll (self-centring, acro style), A/D yaw, W/S throttle. B launches/recalls.
  bf2::DroneController drone;
  bool drone_mode = false;

  // Cascaded Shadow Maps: cascades are refit to the live camera frustum each
  // frame (splits + light-space matrices). The depth-pass rendering is the next
  // integration step; the matrices are already correct and inspectable.
  bf2::CascadedShadowMaps<4> csm;
  float drone_throttle = 0.f;
  float drone_stick_pitch = 0.f;  // per-frame mouse-driven commands
  float drone_stick_roll = 0.f;
  float signal = 1.f;  // FPV link quality 0..1 (drops with range / occlusion)

  Uint64 title_timer = 0;
  int cur_w = win_w;
  int cur_h = win_h;

  // Grass geometry is rebuilt only when the player moves far enough, so it stays
  // stable and cheap. radius ~ Undergrowth.cfg ViewDistance.
  std::vector<float> grass_verts;
  glm::vec2 grass_center(1e9f, 1e9f);
  const float grass_radius = 46.f;

  int frame_no = 0;
  if (shot_third_person && have_soldier) third_person = true;
  if (shot_drone) {  // headless drone screenshot: launch immediately
    drone_mode = true;
    drone = bf2::DroneController{};
    drone.position = glm::vec3(player.position.x, player.position.y + 8.f, player.position.z);
    drone.velocity = glm::vec3(0.f);
    drone_throttle = 0.30f;
    signal = 1.f;
  }

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        running = false;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        running = false;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_TAB) {
        mouse_look = !mouse_look;
        SDL_SetRelativeMouseMode(mouse_look ? SDL_TRUE : SDL_FALSE);
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_v && have_soldier) {
        third_person = !third_person;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_f) {
        ballistic = !ballistic;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_b) {
        // Launch or recall the FPV drone.
        drone_mode = !drone_mode;
        if (drone_mode) {
          drone = bf2::DroneController{};
          const glm::vec3 launch(player.position.x, player.position.y + 0.6f, player.position.z);
          drone.position = launch;
          drone_throttle = 0.30f;  // near hover
          signal = 1.f;
        }
      } else if (e.type == SDL_MOUSEMOTION && mouse_look && drone_mode) {
        drone_stick_roll += e.motion.xrel * 0.020f;
        drone_stick_pitch += e.motion.yrel * 0.020f;
      } else if (!drone_mode && ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q) ||
                                 e.type == SDL_MOUSEWHEEL)) {
        // Cycle to the next weapon that loads successfully (Q or mouse wheel).
        const int dir = (e.type == SDL_MOUSEWHEEL && e.wheel.y < 0) ? -1 : 1;
        for (std::size_t step = 0; step < weapon_defs.size(); ++step) {
          weapon_index = (weapon_index + weapon_defs.size() + dir) % weapon_defs.size();
          if (load_weapon(weapon_index)) break;
        }
      } else if (e.type == SDL_MOUSEMOTION && mouse_look) {
        yaw += e.motion.xrel * sensitivity;
        pitch -= e.motion.yrel * sensitivity;
        pitch = std::clamp(pitch, -89.f, 89.f);
      } else if (e.type == SDL_WINDOWEVENT &&
                 e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        cur_w = e.window.data1;
        cur_h = e.window.data2;
      }
    }

    const Uint64 now = SDL_GetPerformanceCounter();
    float dt = static_cast<float>(now - prev) / static_cast<float>(SDL_GetPerformanceFrequency());
    prev = now;
    dt = std::clamp(dt, 0.f, 0.05f);

    // Camera basis.
    glm::vec3 front;
    front.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    front.y = std::sin(glm::radians(pitch));
    front.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    front = glm::normalize(front);
    glm::vec3 flat_front(0.f, 0.f, -1.f);
    if (const float flat_len2 = front.x * front.x + front.z * front.z; flat_len2 > 1e-6f) {
      flat_front = glm::normalize(glm::vec3(front.x, 0.f, front.z));
    }
    const glm::vec3 right = glm::normalize(glm::cross(flat_front, glm::vec3(0, 1, 0)));

    // Movement.
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
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
      // The operator's body stays put while piloting.
      player.desired_velocity = {0.f, 0.f, 0.f};
      world.step_character(player, dt > 0.f ? dt : 1.f / 60.f);
    } else if (keys != nullptr) {
      glm::vec3 move(0.f);
      if (keys[SDL_SCANCODE_W]) move += flat_front;
      if (keys[SDL_SCANCODE_S]) move -= flat_front;
      if (keys[SDL_SCANCODE_D]) move += right;
      if (keys[SDL_SCANCODE_A]) move -= right;
      const float speed = keys[SDL_SCANCODE_LSHIFT] ? 12.f : 6.5f;
      if (glm::length(move) > 0.001f) {
        move = glm::normalize(move) * speed;
      }
      player.desired_velocity = {move.x, 0.f, move.z};
      if (keys[SDL_SCANCODE_SPACE] && player.on_ground) {
        player.vertical_velocity = 6.5f;
      }
      world.step_character(player, dt > 0.f ? dt : 1.f / 60.f);
    } else {
      player.desired_velocity = {0.f, 0.f, 0.f};
      world.step_character(player, dt > 0.f ? dt : 1.f / 60.f);
    }

    const glm::vec3 eye(player.position.x, player.position.y, player.position.z);

    // Shooting: automatic fire while left mouse is held.
    fire_cooldown = std::max(0.f, fire_cooldown - dt);
    muzzle_flash = std::max(0.f, muzzle_flash - dt);
    recoil = std::max(0.f, recoil - dt * 6.f);
    const Uint32 mouse = SDL_GetMouseState(nullptr, nullptr);
    const bool lmb = (mouse & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    if (lmb && mouse_look && !drone_mode && fire_cooldown <= 0.f) {
      fire_cooldown = 0.1f;  // ~600 rpm
      muzzle_flash = 0.045f;
      recoil = std::min(1.f, recoil + 0.6f);
      const glm::vec3 muzzle = eye + front * 0.6f + right * 0.18f - glm::vec3(0, 1, 0) * 0.12f;
      if (ballistic) {
        // Spawn a physical bullet from the muzzle along the aim direction.
        projectiles.push_back({muzzle, front * kMuzzleSpeed, kBulletLife});
      } else {
        // Hitscan: instant ray to first hit (nearest of terrain/enemy).
        const auto hit = world.raycast({eye.x, eye.y, eye.z}, {front.x, front.y, front.z}, 400.f);
        const float terr = hit.hit ? hit.distance : 400.f;
        const auto eh = shoot_enemies(eye, front, terr);
        if (eh.idx >= 0) {
          damage_enemy(eh.idx, eh.zone);
          tracers.push_back({muzzle, eh.point, 0.06f});
          impacts.push_back({eh.point, 0.4f});
        } else {
          const glm::vec3 end =
              hit.hit ? glm::vec3(hit.point.x, hit.point.y, hit.point.z) : eye + front * 400.f;
          tracers.push_back({muzzle, end, 0.06f});
          if (hit.hit) impacts.push_back({end, 0.6f});
        }
      }
    }

    // Integrate physical projectiles: gravity + a segment raycast per frame so
    // fast bullets can't tunnel through geometry.
    // Gentle gusting so the wind isn't perfectly constant.
    {
      const float t = static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
      const float gust = 0.7f + 0.3f * std::sin(t * 0.37f);
      wind = wind_base * gust;
    }
    for (auto& pr : projectiles) {
      pr.life -= dt;
      if (pr.life <= 0.f) continue;
      pr.vel.y += kBulletGravity * dt;
      // Quadratic drag relative to the air: slows the round and drifts it downwind.
      const glm::vec3 rel = pr.vel - wind;
      const float rspd = glm::length(rel);
      if (rspd > 1e-3f) pr.vel -= rel * (kBulletDrag * rspd * dt);
      const glm::vec3 next = pr.pos + pr.vel * dt;
      const glm::vec3 seg = next - pr.pos;
      const float seg_len = glm::length(seg);
      if (seg_len > 1e-5f) {
        const glm::vec3 sd = seg / seg_len;
        const auto hit = world.raycast({pr.pos.x, pr.pos.y, pr.pos.z},
                                       {seg.x, seg.y, seg.z}, seg_len);
        const float terr = hit.hit ? hit.distance : 1e30f;
        const auto eh = shoot_enemies(pr.pos, sd, seg_len);
        if (eh.idx >= 0 && eh.dist < terr) {
          damage_enemy(eh.idx, eh.zone);
          impacts.push_back({eh.point, 0.4f});
          tracers.push_back({pr.pos, eh.point, 0.05f});
          pr.life = 0.f;
          continue;
        }
        if (hit.hit) {
          impacts.push_back({glm::vec3(hit.point.x, hit.point.y, hit.point.z), 0.6f});
          // Short tracer at the terminal segment, then kill the bullet.
          tracers.push_back({pr.pos, glm::vec3(hit.point.x, hit.point.y, hit.point.z), 0.05f});
          pr.life = 0.f;
          continue;
        }
      }
      pr.pos = next;
    }

    for (auto& t : tracers) t.life -= dt;
    for (auto& im : impacts) im.life -= dt;
    tracers.erase(std::remove_if(tracers.begin(), tracers.end(),
                                 [](const Tracer& t) { return t.life <= 0.f; }),
                  tracers.end());
    impacts.erase(std::remove_if(impacts.begin(), impacts.end(),
                                 [](const Impact& im) { return im.life <= 0.f; }),
                  impacts.end());
    projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(),
                                     [](const Projectile& p) { return p.life <= 0.f; }),
                      projectiles.end());

    // Enemy AI + player health. Enemies acquire the player by line of sight,
    // ramp an "alert" awareness (reaction time), then fire controlled bursts
    // with distance-based accuracy. Only the closest few actually shoot at once
    // so you're never deleted by a whole garrison the instant you're seen.
    player_regen_delay = std::max(0.f, player_regen_delay - dt);
    if (player_regen_delay <= 0.f && player_health < 100.f) {
      player_health = std::min(100.f, player_health + 12.f * dt);
    }
    constexpr float kSightRange = 130.f;   // can see this far in the open
    constexpr float kEngageRange = 100.f;  // will shoot within this range
    constexpr int kMaxShooters = 3;        // simultaneous attackers cap
    // Gather living enemies with LOS, sorted by distance; only the nearest few fire.
    struct Contact { int idx; float dist; };
    std::vector<Contact> contacts;
    for (std::size_t i = 0; i < enemies.size(); ++i) {
      Enemy& en = enemies[i];
      if (!en.alive) {
        en.death_time += dt;
        if (en.death_time > 12.f) {  // respawn after a while
          en.pos = en.home;
          drop(en);
          en.alive = true;
          en.health = 100.f;
          en.hit_flash = 0.f;
          en.alert = 0.f;
          en.burst_left = 0;
          en.burst_cooldown = 1.f + frand();
        }
        continue;
      }
      en.hit_flash = std::max(0.f, en.hit_flash - dt);
      en.anim_time += dt;
      const glm::vec3 chest = en.pos + glm::vec3(0.f, 1.35f, 0.f);
      const glm::vec3 to_player = eye - chest;
      const float dist = glm::length(to_player);
      bool los = false;
      if (dist > 1e-3f && dist < kSightRange) {
        const glm::vec3 dir = to_player / dist;
        const auto hit = world.raycast({chest.x, chest.y, chest.z}, {dir.x, dir.y, dir.z}, dist);
        los = !hit.hit || hit.distance >= dist - 1.5f;
      }
      // Alert ramps while the target is visible (reaction delay), decays otherwise.
      en.alert = std::clamp(en.alert + (los ? dt / 0.8f : -dt / 3.0f), 0.f, 1.f);
      en.moving = false;
      if (los && dist < kEngageRange) {
        en.yaw = std::atan2(to_player.x, to_player.z);
        // Close the gap toward a preferred fighting distance; hold and shoot inside it.
        if (dist > 35.f) {
          const glm::vec3 step =
              glm::normalize(glm::vec3(to_player.x, 0.f, to_player.z)) * 3.0f * dt;
          en.pos.x += step.x;
          en.pos.z += step.z;
          en.moving = true;
        }
        contacts.push_back({static_cast<int>(i), dist});
      } else if (los) {
        // Spotted the player but out of engage range: advance to contact.
        en.yaw = std::atan2(to_player.x, to_player.z);
        const glm::vec3 step = glm::normalize(glm::vec3(to_player.x, 0.f, to_player.z)) * 2.4f * dt;
        en.pos.x += step.x;
        en.pos.z += step.z;
        en.moving = true;
      } else {
        // No contact: idle patrol around the home post so squads feel alive.
        const glm::vec2 to_wp(en.patrol_target.x - en.pos.x, en.patrol_target.z - en.pos.z);
        const float wp_dist = glm::length(to_wp);
        if (wp_dist < 1.2f) {
          en.patrol_wait -= dt;
          if (en.patrol_wait <= 0.f) {
            const float a = frand() * 6.2831853f;
            const float r = 3.f + frand() * 13.f;
            en.patrol_target =
                glm::vec3(en.home.x + std::cos(a) * r, 0.f, en.home.z + std::sin(a) * r);
            en.patrol_wait = 1.5f + frand() * 3.5f;  // pause on arrival next time
          }
        } else {
          const glm::vec3 step = glm::normalize(glm::vec3(to_wp.x, 0.f, to_wp.y)) * 1.5f * dt;
          en.pos.x += step.x;
          en.pos.z += step.z;
          en.yaw = std::atan2(to_wp.x, to_wp.y);
          en.moving = true;
        }
      }
      drop(en);
      // Refresh bone capsules from the current pose for next-frame hit reg.
      const bf2::AnimationClip* clip =
          (en.moving && have_clip_walk) ? &clip_walk : (have_clip_stand ? &clip_stand : nullptr);
      int frame = 0;
      if (clip && clip->frame_count > 0) frame = static_cast<int>(en.anim_time * 30.f) % clip->frame_count;
      update_capsules(en, clip, frame);
    }
    // Let only the nearest shooters actually fire this frame.
    std::sort(contacts.begin(), contacts.end(),
              [](const Contact& a, const Contact& b) { return a.dist < b.dist; });
    for (std::size_t c = 0; c < contacts.size(); ++c) {
      Enemy& en = enemies[contacts[c].idx];
      const float dist = contacts[c].dist;
      const bool can_shoot = static_cast<int>(c) < kMaxShooters && en.alert >= 1.0f;
      en.burst_cooldown = std::max(0.f, en.burst_cooldown - dt);
      if (!can_shoot) continue;
      if (en.burst_left <= 0) {
        if (en.burst_cooldown <= 0.f) {
          en.burst_left = 3 + (std::rand() % 3);  // 3-5 round burst
          en.shot_timer = 0.f;
        }
        continue;
      }
      en.shot_timer -= dt;
      if (en.shot_timer > 0.f) continue;
      en.shot_timer = 0.12f;  // ~500 rpm within a burst
      if (--en.burst_left <= 0) en.burst_cooldown = 1.6f + frand() * 1.6f;  // pause between bursts
      const glm::vec3 chest = en.pos + glm::vec3(0.f, 1.35f, 0.f);
      const glm::vec3 dir = glm::normalize(eye - chest);
      const glm::vec3 muz = chest + dir * 0.3f;
      const float sp = 0.6f + dist * 0.035f;  // spread grows with range
      const glm::vec3 aim =
          eye + glm::vec3((frand() - 0.5f), (frand() - 0.5f), (frand() - 0.5f)) * sp;
      tracers.push_back({muz, aim, 0.05f});
      // Accuracy falls off with distance; damage per round is modest.
      const float phit = std::clamp(0.55f - dist / 220.f, 0.04f, 0.42f);
      if (frand() < phit) {
        player_health -= 6.f + frand() * 5.f;
        player_regen_delay = 3.f;
      }
    }
    if (player_health <= 0.f) {
      player.position = {spawn.x, spawn.y, spawn.z};
      player.vertical_velocity = 0.f;
      player_health = 100.f;
      player_regen_delay = 0.f;
      ++player_deaths;
    }

    // Camera: FPV from the drone when flying, else eye (1st) / behind (3rd).
    glm::vec3 cam = eye;
    glm::vec3 cam_front = front;
    glm::vec3 cam_up(0.f, 1.f, 0.f);
    float cam_fov = 70.f;
    if (drone_mode) {
      cam = drone.position;
      cam_front = drone.forward(25.f);  // FPV cams sit tilted up
      cam_up = drone.up();              // bank with the airframe for immersion
      cam_fov = 95.f;                   // wide FPV lens
      // Link quality: falls with range from the operator and near the ground.
      const float range = glm::distance(cam, eye);
      const float rq = std::clamp(1.f - range / 600.f, 0.f, 1.f);
      signal = std::clamp(signal + (rq - signal) * std::min(dt * 2.f, 1.f), 0.f, 1.f);
    } else if (third_person) {
      const float back = 3.0f;
      const float up = 0.7f;
      cam = eye - front * back + glm::vec3(0, 1, 0) * up;
      const float ground = world.terrain_height(cam.x, cam.z) + 0.3f;
      if (cam.y < ground) cam.y = ground;  // don't sink the camera into terrain
    }
    const glm::mat4 proj = glm::perspective(
        glm::radians(cam_fov), static_cast<float>(cur_w) / std::max(cur_h, 1), 0.2f, 4000.f);
    const glm::mat4 view = glm::lookAt(cam, cam + cam_front, cam_up);
    const glm::mat4 view_proj = proj * view;
    // Refit the shadow cascades to this frame's camera and sun (matrices ready
    // fit the cascades to this frame's camera + sun, then render depth for each.
    csm.update(view, glm::radians(cam_fov), static_cast<float>(cur_w) / std::max(cur_h, 1), 0.2f,
               400.f, atmo.sun_dir);
    const glm::mat4 inv_view_proj = glm::inverse(view_proj);

    // ---- Shadow depth passes: buildings + vehicles cast into each cascade. ----
    const float shadow_cast_dist2 = 170.f * 170.f;  // only nearby casters matter
    float cascade_vp[bf2::Renderer::kShadowCascades * 16];
    float cascade_splits[4];
    for (int c = 0; c < bf2::Renderer::kShadowCascades; ++c) {
      const glm::mat4& vp = csm.cascade(c).view_proj;
      std::memcpy(&cascade_vp[c * 16], glm::value_ptr(vp), 16 * sizeof(float));
      cascade_splits[c] = csm.cascade(c).split_far;
      renderer.begin_shadow_pass(c, glm::value_ptr(vp));
      for (const auto& in : instances) {
        if (glm::dot(in.origin - cam, in.origin - cam) > shadow_cast_dist2) continue;
        const auto mesh_it = template_cache.find(in.mesh_key);
        if (mesh_it == template_cache.end() || mesh_it->second.vao == 0) continue;
        renderer.draw_depth(mesh_it->second, glm::value_ptr(in.model));
      }
      for (const auto& v : vehicles) {
        if (glm::dot(v.origin - cam, v.origin - cam) > shadow_cast_dist2) continue;
        const auto vit = vehicle_cache.find(v.mesh_key);
        if (vit == vehicle_cache.end() || vit->second.vao == 0) continue;
        renderer.draw_depth(vit->second, glm::value_ptr(v.model));
      }
    }
    renderer.set_shadows(cascade_vp, cascade_splits, true);

    const float frame_t =
        static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
    renderer.set_viewport(cur_w, cur_h);
    if (drone_mode) {
      renderer.begin_scene(cur_w, cur_h, atmo.horizon_color.x, atmo.horizon_color.y,
                           atmo.horizon_color.z);
    } else {
      renderer.begin_frame(atmo.horizon_color.x, atmo.horizon_color.y, atmo.horizon_color.z);
    }

    // Per-frame environment: camera position + sun + horizon fog. Fog fades far
    // terrain/objects/water into the horizon colour so map edges disappear.
    renderer.set_environment(glm::value_ptr(cam), glm::value_ptr(atmo.sun_dir),
                             glm::value_ptr(atmo.horizon_color), atmo.fog_start, atmo.fog_end);

    // Gradient sky behind everything (does not write depth).
    renderer.draw_sky(glm::value_ptr(inv_view_proj), glm::value_ptr(cam),
                      glm::value_ptr(atmo.sky_color), glm::value_ptr(atmo.horizon_color));

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
      if (glm::dot(d, d) > draw_dist2) {
        continue;
      }
      const auto mesh_it = template_cache.find(in.mesh_key);
      if (mesh_it == template_cache.end() || mesh_it->second.vao == 0) {
        continue;
      }
      const glm::mat4 mvp = view_proj * in.model;
      renderer.draw_textured(mesh_it->second, glm::value_ptr(mvp), glm::value_ptr(in.model),
                             in.lightmap, glm::value_ptr(in.lm_xform));
      ++drawn;
    }

    // Vehicles (static props at their spawn points).
    for (const auto& v : vehicles) {
      const glm::vec3 d = v.origin - cam;
      if (glm::dot(d, d) > draw_dist2) continue;
      const auto vit = vehicle_cache.find(v.mesh_key);
      if (vit == vehicle_cache.end() || vit->second.vao == 0) continue;
      const glm::mat4 mvp = view_proj * v.model;
      renderer.draw_textured(vit->second, glm::value_ptr(mvp), glm::value_ptr(v.model));
      ++drawn;
    }

    // Third-person animated soldier: pick a locomotion clip by movement speed and
    // drive the GPU skinning palette; body is placed at the player's feet facing
    // the look direction.
    if (third_person && have_soldier) {
      const float spd = std::sqrt(player.desired_velocity.x * player.desired_velocity.x +
                                  player.desired_velocity.z * player.desired_velocity.z);
      const bool moving = spd > 0.3f;
      const bool running = spd > 8.0f;
      const bf2::AnimationClip* clip = nullptr;
      if (running && have_clip_run) {
        clip = &clip_run;
      } else if (moving && have_clip_walk) {
        clip = &clip_walk;
      } else if (have_clip_stand) {
        clip = &clip_stand;
      }

      constexpr float kAnimFps = 30.f;
      anim_time += dt;
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
        const bf2::AnimationClip* clip = nullptr;
        if (en.alive && en.moving && have_clip_walk) {
          clip = &clip_walk;
        } else if (have_clip_stand) {
          clip = &clip_stand;
        }
        int frame = 0;
        if (clip && clip->frame_count > 0) {
          frame = static_cast<int>(en.anim_time * 30.f) % clip->frame_count;
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

    // Grass: alpha-tested billboards scattered on the ground near the player.
    // Rebuilt only when the player has walked far enough from the last patch.
    if (undergrowth.valid() && grass_atlas_tex != 0) {
      const float t_sec =
          static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
      if (std::abs(cam.x - grass_center.x) > 6.f || std::abs(cam.z - grass_center.y) > 6.f) {
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
      renderer.draw_water(glm::value_ptr(view_proj), atmo.water_level, cam.x, cam.z, 3000.f, t_sec,
                          glm::value_ptr(atmo.water_color));
    }

    // World-space effects: tracers (bright lines) and bullet impacts (crosses).
    if (!tracers.empty()) {
      std::vector<float> line_verts;
      line_verts.reserve(tracers.size() * 6);
      for (const auto& t : tracers) {
        line_verts.insert(line_verts.end(), {t.a.x, t.a.y, t.a.z, t.b.x, t.b.y, t.b.z});
      }
      renderer.draw_lines(glm::value_ptr(view_proj), line_verts.data(),
                          static_cast<int>(line_verts.size() / 3), 1.0f, 0.85f, 0.35f, 2.5f, true);
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

    // First-person weapon viewmodel: rendered in view space over the scene with a
    // narrower FOV so it doesn't distort. The depth buffer is cleared first so
    // the weapon never clips into the world but still self-occludes correctly.
    if (!third_person && !drone_mode) {
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

      // Muzzle flash: bright star at the barrel tip (model-space muzzle location).
      if (muzzle_flash > 0.f) {
        const glm::vec3 tip = have_weapon_model ? glm::vec3(0.52f, -0.52f, -1.05f)
                                                : glm::vec3(0.17f, -0.10f, -1.25f);
        const float f = 0.05f + muzzle_flash * 3.0f;
        std::vector<float> flash = {
            tip.x - f, tip.y, tip.z, tip.x + f, tip.y, tip.z,
            tip.x, tip.y - f, tip.z, tip.x, tip.y + f, tip.z,
            tip.x - f * 0.7f, tip.y - f * 0.7f, tip.z, tip.x + f * 0.7f, tip.y + f * 0.7f, tip.z,
            tip.x - f * 0.7f, tip.y + f * 0.7f, tip.z, tip.x + f * 0.7f, tip.y - f * 0.7f, tip.z,
        };
        renderer.draw_lines(glm::value_ptr(vm_proj), flash.data(),
                            static_cast<int>(flash.size() / 3), 1.0f, 0.9f, 0.4f, 3.0f, false);
      }
    }

    // Resolve the offscreen drone view through the FPV post-process (chromatic
    // aberration, scanlines, static, tearing) scaled by signal loss. In infantry
    // mode the scene was drawn straight to the backbuffer (keeps MSAA).
    if (drone_mode) {
      renderer.present_scene(1.f - signal, frame_t);
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
        // Centre reticle (drone crosshair box).
        const float b = 14.f;
        std::vector<float> box = {cx - b, cy - b, 0.f, cx + b, cy - b, 0.f, cx + b, cy - b, 0.f,
                                  cx + b, cy + b, 0.f, cx + b, cy + b, 0.f, cx - b, cy + b, 0.f,
                                  cx - b, cy + b, 0.f, cx - b, cy - b, 0.f};
        renderer.draw_lines(glm::value_ptr(ortho), box.data(), 8, 0.3f, 1.0f, 0.4f, 1.5f, false);

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

        // Health bar: dark background + coloured fill at bottom-left.
        const float bx = 24.f, by = 24.f, bw = 220.f;
        std::vector<float> bg = {bx, by, 0.f, bx + bw, by, 0.f};
        renderer.draw_lines(glm::value_ptr(ortho), bg.data(), 2, 0.12f, 0.12f, 0.12f, 12.f, false);
        const float frac = std::clamp(player_health / 100.f, 0.f, 1.f);
        if (frac > 0.f) {
          std::vector<float> fg = {bx, by, 0.f, bx + bw * frac, by, 0.f};
          const float rr = frac < 0.4f ? 0.9f : 0.2f;
          const float gg = frac < 0.4f ? 0.2f : 0.85f;
          renderer.draw_lines(glm::value_ptr(ortho), fg.data(), 2, rr, gg, 0.2f, 10.f, false);
        }
      }
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
                      "Project Dalian  |  FPV DRONE  |  batt %.0f%%  thr %.0f%%  sig %.0f%%  |  %.0f fps  |"
                      "  mouse pitch/roll, A/D yaw, W/S throttle, B recall, ESC",
                      drone.battery * 100.f, drone_throttle * 100.f, signal * 100.f,
                      dt > 0 ? 1.f / dt : 0.f);
      } else {
        std::snprintf(title, sizeof(title),
                      "Project Dalian  |  %s  |  %s  |  %s  |  HP %.0f  |  kills %d  deaths %d  |  %.0f fps"
                      "  |  LMB fire, B drone, F ballistic, V 1st/3rd, WASD, TAB, ESC",
                      have_weapon_model ? weapon_defs[weapon_index].name : "gun",
                      ballistic ? "ballistic" : "hitscan", third_person ? "3rd" : "1st",
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
  if (have_ground_tex) {
    renderer.destroy_textured(terrain_tex_gpu);
    bf2::TerrainColormapLoader::destroy(ground_tex);
  } else {
    renderer.destroy_mesh(terrain_gpu);
  }
  textures.clear();
  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
