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
// LMB fire, wheel/Q swap weapon, F ballistic/hitscan, R launch missile
// (from nearest vehicle), B launch/recall FPV recon drone, N launch FPV
// kamikaze loitering munition (one-way), ESC pause menu (Alt+F4 to quit).
#include <GL/glew.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "app_settings.hpp"
#include "ui_layout.hpp"
#include "factions.hpp"
#include "game_audio.hpp"
#include "menu.hpp"
#include "server_discovery.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
#include <vector>

#include "engine/anim/pose.hpp"
#include "engine/anim/skinning.hpp"
#include "engine/core/atmosphere.hpp"
#include "engine/core/level_loader.hpp"
#include "engine/core/object_lightmaps.hpp"
#include "engine/core/undergrowth.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"
#include "engine/net/net.hpp"
#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/collision/bf2_collision.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
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
  dalian::sync_drawable_size(window, settings.width, settings.height);

  bf2::Renderer renderer;
  renderer.initialize(window);
  renderer.set_viewport(settings.width, settings.height);
  dalian::apply_graphics_settings(renderer, settings);
  renderer.reload_shadow_res(settings.shadow_res);

  const std::string bf2_root =
      dalian::resolve_bf2_root(settings, argc >= 2 ? argv[1] : nullptr);
  std::vector<dalian::MapEntry> map_list = dalian::scan_maps(bf2_root);
  std::cout << "Found " << map_list.size() << " maps under " << (bf2_root.empty() ? "?" : bf2_root)
            << '\n';

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
      atmo.fog_start = std::max(atmo.fog_start, 1200.f) * fog_scale;
      atmo.fog_end = std::max(atmo.fog_end, 6500.f) * fog_scale;
    }
    std::cout << "Atmosphere: sky " << atmo.sky_color.x << "/" << atmo.sky_color.y << "/"
              << atmo.sky_color.z << (atmo.has_water ? "  water level " : "  (no water")
              << (atmo.has_water ? std::to_string(atmo.water_level) : std::string(")"))
              << "  fog " << atmo.fog_start << ".." << atmo.fog_end << '\n';

    bf2::TemplateResolver resolver(resources);
    if (const auto so = resources.read_bytes("StaticObjects.con")) {
      const std::string script(reinterpret_cast<const char*>(so->data()), so->size());
      resolver.build_from_static_objects(script);
    }
    std::cout << "Resolved " << resolver.map().size() << " templates; " << level.placements.size()
              << " placements\n";

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
            if (cov > 0.7 || cov < 0.005 || solid > 0.015) {
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
            // Foliage/fence leaf cards use an alpha cutout mask; flag them so the
            // renderer discards the transparent texels instead of drawing white.
            gpu.submeshes[i].cutout = textures.is_cutout(gpu.submeshes[i].base_tex);
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
  std::string gameplay_script;   // layer used for control-point/player spawns
  std::string vehicle_script;    // layer used for vehicle placement (richest set)
  std::vector<glm::vec3> control_points;  // above-water objectives, for enemy placement
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
  }
  player.position = {spawn.x, spawn.y, spawn.z};
  std::cout << "Spawn @ " << spawn.x << " " << spawn.y << " " << spawn.z << '\n';

  // Vehicles: placed by ObjectSpawners in the gameplay layout. A BF2 vehicle is
  // assembled from a root bundledmesh (its 3rd-person "geom1" external body) plus
  // separate child meshes (rotors, sometimes turrets) attached at offsets defined
  // in the vehicle's .tweak/.con. Parts internal to the body (wings, gun turret,
  // landing skids) are already baked into the body mesh as geometry parts.
  struct VehiclePart {
    std::string mesh_key;
    glm::mat4 local{1.0f};  // relative to the vehicle root
  };
  struct VehicleWheelSlot {
    int geom_part = -1;
    std::string mesh_key;
    glm::mat4 rest{1.f};
    bool steers = false;
  };
  struct Vehicle {
    std::string mesh_key;  // root body
    glm::mat4 model{1.0f};
    glm::vec3 origin{};
    std::vector<VehiclePart> parts;  // child meshes (rotors, etc.)
    // Live driving state (see the E-to-enter system below).
    glm::vec3 pos{};       // current world position (starts at origin)
    float heading = 0.f;   // yaw in degrees
    float speed = 0.f;     // signed ground speed (m/s)
    bool is_air = false;   // helicopters / jets use the simple flight model
    float rotor_spin = 0.f;  // accumulated rotor angle (rad) while flying
    float rotor_rpm = 0.f;   // 0..1 spool state: 0 parked (static blades), 1 full (blur disc)
    // Helicopter flight attitude (degrees) and full 3D velocity. Cyclic pitch
    // tilts the nose (accelerate forward), roll banks into coordinated turns.
    float pitch = 0.f;
    float roll = 0.f;
    glm::vec3 vel{0.f};
    bool has_gunner_seat = false;  // attack helis carry a co-pilot/gunner station
    bool is_heli = false;          // rotary-wing: hovers (collective). false = fixed-wing jet
    float throttle = 0.f;          // jet engine setting 0..1 (W spools up, S airbrakes)
    bool wheels_on_ground = true;  // jet: true while gear is on the deck/runway
    // Crew seats (BF2-style). occupant: -1 empty, 0 the player, 1 an AI crewman.
    // Seat 0 is always the driver/pilot; extra seats are gunner/passenger.
    struct SeatSlot {
      const char* name = "SEAT";
      int occupant = -1;
    };
    std::vector<SeatSlot> seats;
    // Smoothed chassis up-vector: ground vehicles tilt to follow the terrain /
    // surface slope (a cheap suspension model). Aircraft keep +Y.
    glm::vec3 ground_normal{0.f, 1.f, 0.f};
    // Ground clearance: distance the lowest vertex sits below the mesh origin.
    // Vehicles are placed/snapped so origin.y = terrain + clearance, which lands
    // the wheels/tracks/skids on the ground instead of sinking the hull into it.
    float clearance = 0.f;
    // Landing clearance for aircraft: the true skid/gear height above ground.
    // `clearance` may include a large scripted offset (e.g. a carrier deck), so
    // the in-flight terrain floor uses this smaller value to let aircraft land.
    float land_clearance = 0.f;
    // Oriented collision half-extents (x=right, y=height, z=forward) in local
    // space, for pushing the on-foot player out of the hull.
    glm::vec3 col_half{1.6f, 1.4f, 3.0f};
    std::vector<VehicleWheelSlot> wheels;
    std::vector<float> wheel_spin;
    float visual_steer = 0.f;
  };
  std::vector<Vehicle> vehicles;
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
      std::vector<VehicleWheelSlot> wheels;
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
                                  const std::string& log_vpath) -> bool {
        if (data.vertices.empty()) return false;
        std::vector<float> ys;
        ys.reserve(data.vertices.size());
        for (const auto& vtx : data.vertices) ys.push_back(vtx.position.y);
        const std::size_t k =
            std::min(ys.size() - 1, static_cast<std::size_t>(ys.size() * 0.015f));
        std::nth_element(ys.begin(), ys.begin() + k, ys.end());
        vehicle_min_y[key] = ys[k];
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
        int missing = 0;
        for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
          gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
          gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
          gpu.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map);
          gpu.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map);
          gpu.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map);
          if (gpu.submeshes[i].base_tex == 0) ++missing;
        }
        if (missing > 0 && std::getenv("BF2_VEHLIST"))
          std::cerr << "  TEXMISS " << bf2::detail::basename_no_ext(log_vpath) << ": " << missing
                    << "/" << gpu.submeshes.size() << " submeshes missing base texture\n";
        const bool ok = gpu.vao != 0;
        vehicle_cache[key] = std::move(gpu);
        return ok;
      };

      bf2::GpuTexturedMesh gpu;
      try {
        const auto mesh = resources.load_mesh(vpath);
        const std::size_t geom =
            geom_override >= 0 ? static_cast<std::size_t>(geom_override) : external_geometry(mesh);
        auto data = bf2::MeshLoader::extract_textured(mesh, geom, 0);
        const auto& part_xform = hierarchy ? hierarchy->xforms : std::vector<glm::mat4>{};
        std::unordered_set<int> wheel_parts;
        if (hierarchy) {
          for (const auto& w : hierarchy->wheels) wheel_parts.insert(w.geom_part);
        }
        if (!part_xform.empty() && data.vertex_part.size() == data.vertices.size() &&
            !wheel_parts.empty()) {
          bf2::TexturedMeshData body;
          std::unordered_map<int, bf2::TexturedMeshData> wheel_meshes;
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
          for (std::size_t ii = 0; ii + 2 < data.indices.size(); ii += 3) {
            const std::uint32_t ia = data.indices[ii];
            const int p = static_cast<int>(data.vertex_part[ia]);
            if (wheel_parts.count(p)) {
              auto& wd = wheel_meshes[p];
              wd.indices.push_back(map_vert(p, ia, wd));
              wd.indices.push_back(map_vert(p, data.indices[ii + 1], wd));
              wd.indices.push_back(map_vert(p, data.indices[ii + 2], wd));
            } else {
              body.indices.push_back(map_vert(-1, ia, body));
              body.indices.push_back(map_vert(-1, data.indices[ii + 1], body));
              body.indices.push_back(map_vert(-1, data.indices[ii + 2], body));
            }
          }
          auto assign_submesh = [&](bf2::TexturedMeshData& target) {
            target.submeshes.clear();
            if (target.indices.empty()) return;
            bf2::TexturedSubmesh sm;
            sm.index_offset = 0;
            sm.index_count = static_cast<std::uint32_t>(target.indices.size());
            if (!data.submeshes.empty()) {
              sm.base_map = data.submeshes[0].base_map;
              sm.detail_map = data.submeshes[0].detail_map;
              sm.normal_map = data.submeshes[0].normal_map;
              sm.dirt_map = data.submeshes[0].dirt_map;
              sm.crack_map = data.submeshes[0].crack_map;
            }
            target.submeshes.push_back(sm);
          };
          assign_submesh(body);
          bool ok = upload_mesh_data(body, cache_key, vpath);
          for (const auto& w : hierarchy->wheels) {
            auto wit = wheel_meshes.find(w.geom_part);
            if (wit == wheel_meshes.end() || wit->second.vertices.empty()) continue;
            assign_submesh(wit->second);
            const std::string wkey = vpath + "#wheel_" + std::to_string(w.geom_part);
            upload_mesh_data(wit->second, wkey, vpath);
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
      // vehicles/<cat>/<name>/meshes/<name>.bundledmesh -> folder vehicles/<cat>/<name>
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
    // BF2 vehicles bake turret/barrel/wheels into one bundledmesh as separate
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
               s.find("tyre") != std::string::npos || s.find("track") != std::string::npos ||
               s.find("sprocket") != std::string::npos;
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
          objs[cur];
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
          if (is_wheel_name(oname)) {
            VehicleWheelSlot ws;
            ws.geom_part = gp;
            ws.rest = xform;
            ws.steers = is_steer_name(oname);
            hierarchy.wheels.push_back(std::move(ws));
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
      v.wheel_spin.assign(v.wheels.size(), 0.f);
      v.is_air = vpath.find("vehicles/air/") != std::string::npos;
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
      const float script_off = vs.pos.y - surfY;
      const auto myit = vehicle_min_y.find(vpath);
      const float body_low = myit != vehicle_min_y.end() ? myit->second : 0.f;
      // Lowest rendered point including child parts (wheels are often separate
      // parts, not in the body mesh).
      float low = body_low;
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
      v.heading = vs.rot.x;
      glm::mat4 m = glm::translate(glm::mat4(1.0f), place);
      m = glm::rotate(m, glm::radians(vs.rot.x), glm::vec3(0, 1, 0));
      m = glm::rotate(m, glm::radians(vs.rot.y), glm::vec3(1, 0, 0));
      m = glm::rotate(m, glm::radians(vs.rot.z), glm::vec3(0, 0, 1));
      v.model = m;
      v.parts = pcit->second;
      // A rotor part means this is a helicopter (not a jet): it flies the cyclic
      // model well and carries a co-pilot/gunner station.
      for (const auto& p : v.parts) {
        if (p.mesh_key.find("rotor") != std::string::npos) {
          v.has_gunner_seat = true;
          v.is_heli = true;
          break;
        }
      }
      // Crew seat layout by vehicle class. Seat 0 always drives/pilots.
      const bool is_tank = vpath.find("vehicles/land/") != std::string::npos &&
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
      vehicles.push_back(std::move(v));
      ++placed;
    }
    std::cout << "Vehicles: " << placed << " placed, " << vehicle_cache.size()
              << " unique meshes, " << part_count << " attached parts\n";
    if (std::getenv("BF2_VEHLIST")) {
      for (const auto& v : vehicles)
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
      if (const auto amb = resources.read_bytes("ambientobjects.con")) {
        game_audio.start_level_ambient(
            resources, std::string(reinterpret_cast<const char*>(amb->data()), amb->size()));
      }
      std::unordered_set<std::string> seen_veh_snd;
      for (const auto& vv : vehicles) {
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
  float voice_cooldown = 0.f;

  bf2::GpuColorMesh gun_mesh{};
  if (!have_weapon_model) {
    std::vector<float> gv;
    std::vector<std::uint32_t> gi;
    build_gun_mesh(gv, gi);
    gun_mesh = renderer.upload_color(gv, gi);
  }

  // ---- Vehicle-launched missile (9M311 / SA-19 "Grison"). Loaded exactly like
  // a weapon: locate its .bundledmesh by name, upload the richest geometry, and
  // resolve its _c/_deb/... textures. Falls back to any missile/rocket mesh, and
  // if none is present the launcher draws a procedural streak instead. ----
  bf2::GpuTexturedMesh missile_mesh{};
  {
    const std::vector<const char*> pref = {"ru_sam_9m311", "9m311", "ru_sam_sa18"};
    const auto all = resources.archives().list();
    auto is_bundled = [](const std::string& p) {
      return p.size() > 12 && p.compare(p.size() - 12, 12, ".bundledmesh") == 0;
    };
    std::string mpath;
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
          }
        }
      } catch (const std::exception&) {
      }
    }
    std::cout << "Missile: "
              << (missile_mesh.vao != 0 ? mpath : std::string("procedural (no mesh found)")) << '\n';
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
              << "/" << (have_clip_stand ? clip_stand.bone_count : -1) << "b"
              << " walk=" << (have_clip_walk ? clip_walk.frame_count : -1)
              << "/" << (have_clip_walk ? clip_walk.bone_count : -1) << "b"
              << " run=" << (have_clip_run ? clip_run.frame_count : -1)
              << "/" << (have_clip_run ? clip_run.bone_count : -1) << "b"
              << " (ske nodes=" << soldier_ske.nodes.size() << ")\n";
  }
  bool third_person = false;
  float anim_time = 0.f;

  // Vehicle occupancy: index into `vehicles`, or -1 when on foot.
  int in_vehicle = -1;
  int player_seat = 0;  // which crew seat the player occupies in `in_vehicle`
  // Virtual flight stick (BF2 mouse-flight): while piloting an aircraft the mouse
  // deflects this stick instead of free-looking. X rolls, Y pitches; A/D yaw.
  // Helicopters read it as an attitude command (tilt holds); jets read it as a
  // pitch/roll RATE (so you can loop and barrel-roll).
  float air_pitch_stick = 0.f;  // -1..1
  float air_roll_stick = 0.f;   // -1..1
  float air_input_grace = 0.f;  // ignore mouse briefly after entering (SDL warp spike)
  bool air_stick_moved = false; // set when mouse moves the flight stick this frame
  // Invert flight pitch when enabled in settings.
  auto air_invert_fn = [&]() { return settings.invert_air; };

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

  // ---- Vehicle-launched missiles ------------------------------------------
  // A live missile wraps the reusable MissileController flight model, plus an
  // optional homing target (a tracked enemy) and a smoke-spawn timer.
  struct ActiveMissile {
    bf2::MissileController m;
    int homing_enemy = -1;  // >=0: retarget onto this enemy each frame
    glm::vec3 prev_pos{};
    float smoke_timer = 0.f;
  };
  std::vector<ActiveMissile> missiles;
  // Drifting smoke puff (rocket exhaust trail + launch cloud). Expands and fades.
  struct Smoke {
    glm::vec3 p{};
    float age = 0.f;
    float life = 1.6f;
  };
  std::vector<Smoke> smoke;
  // Explosion flash: an expanding bright star that fades over its life.
  struct Explosion {
    glm::vec3 p{};
    float age = 0.f;
    float life = 0.6f;
  };
  std::vector<Explosion> explosions;
  float missile_reload = 0.f;  // seconds until the launcher can fire again

  // ---- Helicopter weapons -------------------------------------------------
  // Pilot fires forward rocket pods (LMB), the co-pilot/gunner auto-engages
  // ground targets with a chin cannon + rockets, and flares (X) punch out a
  // countermeasure burst that decoys guided threats.
  float heli_rocket_cd = 0.f;  // pilot rocket-pod ripple cooldown
  float heli_gun_cd = 0.f;     // AI gunner cannon cooldown
  float heli_grocket_cd = 0.f; // AI gunner rocket cooldown
  float heli_flare_cd = 0.f;   // flare dispenser cooldown
  // Gunner-station state (AI or player). Tracks a locked target and how long it
  // has been held so the gun doesn't chatter the instant anything appears, and
  // so the HUD can show SCANNING vs ENGAGING honestly.
  int gunner_target = -1;      // enemy index the gunner is tracking, or -1
  float gunner_acquire = 0.f;  // seconds the current target has been in sight
  bool gunner_engaging = false;// true only on frames the gun/rockets actually fire
  struct Flare {
    glm::vec3 p{};
    glm::vec3 v{};
    float life = 0.f;
  };
  std::vector<Flare> flares;

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
  // Sea surface height (or far below everything when the level has no water). Used
  // everywhere spawns are resolved so neither the player nor bots ever end up
  // standing on the sea floor beneath the water plane.
  const float water_y = atmo.has_water ? atmo.water_level : -1e9f;
  // Resting surface at (x,z): terrain, or a solid deck above it near `refy`
  // (carrier flight deck / bridge). Shared by enemy placement and post filtering.
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
  auto drop = [&](Enemy& en) {  // snap feet to the ground — or a deck above it
    const float terr = world.terrain_height(en.pos.x, en.pos.z);
    float y = terr;
    // If the post sits on a raised deck (carrier/bridge), probe downward from the
    // post height so defenders stand ON the deck, not on the sea floor under it.
    const float ref = en.home.y;
    if (ref > terr + 1.0f) {
      const auto dn =
          world.raycast({en.pos.x, ref + 8.f, en.pos.z}, {0.f, -1.f, 0.f}, 60.f);
      if (dn.hit && std::fabs(dn.normal.y) > 0.4f && dn.point.y > terr + 1.0f &&
          dn.point.y <= ref + 8.5f) {
        y = dn.point.y;
      }
    }
    en.pos.y = y;
  };
  if (have_soldier) {
    // Garrison the objectives: a small squad defends each above-water control
    // point except the one the player spawns on. Enemies are spread out around
    // their post, so you meet a few at a time as you advance across the map.
    const float min_from_spawn = 55.f;  // keep the immediate spawn area safe
    // The map's spawn markers are dense (dozens of points), so garrisoning every
    // one would blanket the level in hundreds of enemies. Thin them to a handful
    // of well-separated objectives and cap the total force so fights stay sparse
    // and readable, more like real infantry contact than a firing line.
    constexpr float kMinPostSpacing = 90.f;  // objectives at least this far apart
    constexpr std::size_t kMaxPosts = 10;    // distinct garrisoned objectives
    constexpr std::size_t kMaxEnemies = 28;  // hard cap on total defenders
    std::vector<glm::vec3> candidates;
    for (const auto& cp : control_points) {
      if (glm::distance(glm::vec2(cp.x, cp.z), glm::vec2(spawn.x, spawn.z)) < min_from_spawn) continue;
      // Skip objectives whose ground sits below the sea surface — garrisoning them
      // would drop defenders underwater on the sea floor.
      if (ground_surface(cp.x, cp.z, cp.y) < water_y - 0.3f) continue;
      candidates.push_back(cp);
    }
    std::shuffle(candidates.begin(), candidates.end(),
                 std::mt19937{std::random_device{}()});
    std::vector<glm::vec3> posts;
    for (const auto& cp : candidates) {
      bool too_close = false;
      for (const auto& p : posts) {
        if (glm::distance(glm::vec2(cp.x, cp.z), glm::vec2(p.x, p.z)) < kMinPostSpacing) {
          too_close = true;
          break;
        }
      }
      if (too_close) continue;
      posts.push_back(cp);
      if (posts.size() >= kMaxPosts) break;
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
      if (enemies.size() >= kMaxEnemies) break;
      const int squad = 2 + (std::rand() % 2);  // 2-3 defenders per objective
      for (int i = 0; i < squad; ++i) {
        Enemy en;
        const float ang = frand() * 6.2831853f;
        const float r = frand() * 22.f;  // dispersed around the objective
        en.home = glm::vec3(post.x + std::cos(ang) * r, post.y, post.z + std::sin(ang) * r);
        en.pos = en.home;
        en.patrol_target = en.home;
        drop(en);
        // Never place a defender below the sea surface (dispersal may land one on a
        // beach that dips underwater); skip it rather than drown it.
        if (en.pos.y < water_y - 0.3f) continue;
        en.home.y = en.pos.y;
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

  // Area-of-effect blast: damage every living enemy within `radius` of the
  // detonation, falling off with distance. Used by missile impacts.
  auto explode_at = [&](const glm::vec3& center, float radius, float max_damage) {
    for (auto& en : enemies) {
      if (!en.alive) continue;
      const glm::vec3 chest(en.pos.x, en.pos.y + 1.0f, en.pos.z);
      const float d = glm::length(chest - center);
      if (d > radius) continue;
      const float falloff = 1.f - (d / radius);
      en.health -= max_damage * falloff * falloff;
      en.hit_flash = 0.15f;
      en.alert = 1.f;
      if (en.health <= 0.f) {
        en.alive = false;
        en.death_time = 0.f;
        ++player_kills;
      }
    }
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
  bool mouse_look = true;
  bool pause_open = false;

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
  }

  Uint64 prev = SDL_GetPerformanceCounter();
  // Object draw distance (tunable live from the pause/options menu).
  float draw_dist = settings.draw_distance;
  float draw_dist2 = draw_dist * draw_dist;

  // FPV drone: a 6-DoF quadcopter you can launch and fly. Mouse steers pitch/
  // roll (self-centring, acro style), A/D yaw, W/S throttle. B = recon (recall
  // with B). N = kamikaze loitering munition (one-way, detonates on impact).
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

  auto detonate_kamikaze = [&](const glm::vec3& pt) {
    explode_at(pt, 6.f, 130.f);
    explosions.push_back({pt, 0.f, 0.55f});
    for (int i = 0; i < 8; ++i) smoke.push_back({pt, 0.f, 1.4f});
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
  float grass_radius = 120.f;
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
  float deploy_faction_scroll = 0.f;

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
      {"Anti-Tank", {0, 2}, 1, 0, false, true, "AT Rocket (T)"},
      {"Sniper", {5, 3}, 1, 1, false, false, "Claymore x1"},
  };

  const int kMagSize = 30;
  const int kReserveMags = 6;
  int mag_ammo = kMagSize;
  int reserve_ammo = kMagSize * kReserveMags;
  int grenades_left = 0;
  int c4_left = 0;
  bool has_medkit = false;
  bool has_at = false;
  float player_stamina = 100.f;
  float reload_timer = 0.f;
  bool reloading = false;
  float medkit_cd = 0.f;

  int selected_kit = 0;  // highlighted in the deploy screen
  int player_kit = 0;    // active kit after deploy

  // Named spawn points derived from the map's control points (BF2 flags),
  // thinned so they're well separated across the level.
  struct SpawnPoint {
    std::string name;
    glm::vec3 pos;
  };
  std::vector<SpawnPoint> spawn_points;
  {
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
      spawn_points.push_back({phon[i % 12], chosen[i]});
  }
  int selected_spawn = 0;
  {
    float best = 1e30f;
    for (std::size_t i = 0; i < spawn_points.size(); ++i) {
      const float d = glm::distance(glm::vec2(spawn_points[i].pos.x, spawn_points[i].pos.z),
                                    glm::vec2(spawn.x, spawn.z));
      if (d < best) {
        best = d;
        selected_spawn = static_cast<int>(i);
      }
    }
  }

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
    mag_ammo = kMagSize;
    reserve_ammo = kMagSize * kReserveMags;
    grenades_left = k.grenades;
    c4_left = k.c4;
    has_medkit = k.medic;
    has_at = k.at;
    reloading = false;
    reload_timer = 0.f;
    if (!spawn_points.empty()) {
      const glm::vec3 safe = find_safe_spawn(spawn_points[selected_spawn].pos);
      player.position = {safe.x, safe.y + player.eye_height + 0.5f, safe.z};
      player.vertical_velocity = 0.f;
    }
    player_health = 100.f;
    player_stamina = 100.f;
    live_grenades.clear();
    placed_c4.clear();
  };
  apply_loadout();  // sensible defaults so the weapon/ammo exist behind the menu

  std::string map_label = std::filesystem::path(archive_path).parent_path().filename().string();
  for (char& c : map_label)
    if (c == '_') c = ' ';

  auto rect_hit = [&](int mx, int my, float x, float y, float w, float h) {
    return dalian::ui_hit(renderer, mx, my, x, y, w, h);
  };

  // Draws the deploy screen and (when clicked) handles kit/spawn/deploy picks.
  auto run_deploy_ui = [&](int mx, int my, bool clicked) {
    constexpr float W = 1600.f, H = 900.f;
    renderer.ui_rect(0, 0, W, H, 0.05f, 0.06f, 0.08f, 0.86f);
    const auto& fac_def = dalian::faction_at(player_faction_id);
    const char* fac = fac_def.army;
    renderer.ui_text(40, 26, 3.0f, "DEPLOYMENT", 0.92f, 0.94f, 0.97f, 1.f);
    renderer.ui_text(40, 66, 2.0f, fac_def.country, 0.55f, 0.75f, 1.0f, 1.f);
    renderer.ui_text(40, 92, 1.4f, fac, 0.55f, 0.65f, 0.85f, 1.f);
    renderer.ui_text(40, 114, 1.2f, map_label.c_str(), 0.55f, 0.57f, 0.62f, 1.f);

    // Faction picker (choose who you fight for).
    const float fx = 40, fy = 132, fw = 260, fh = 180;
    renderer.ui_text(fx, fy - 22, 1.4f, "SELECT FACTION", 0.7f, 0.72f, 0.76f, 1.f);
    renderer.ui_rect(fx, fy, fw, fh, 0.04f, 0.05f, 0.06f, 0.95f);
    const float frow = 24.f;
    deploy_faction_scroll =
        std::clamp(deploy_faction_scroll, 0.f,
                   std::max(0.f, static_cast<float>(dalian::faction_count()) * frow - fh));
    const int fstart = static_cast<int>(deploy_faction_scroll / frow);
    for (std::size_t i = fstart; i < dalian::faction_count() &&
                                static_cast<int>(i) < fstart + static_cast<int>(fh / frow) + 2;
         ++i) {
      const float ry = fy + static_cast<float>(i) * frow - deploy_faction_scroll;
      if (ry < fy || ry > fy + fh - frow) continue;
      const bool sel = static_cast<int>(i) == player_faction_id;
      const bool hov = rect_hit(mx, my, fx, ry, fw, frow);
      renderer.ui_rect(fx, ry, fw, frow - 2, sel ? 0.16f : (hov ? 0.11f : 0.07f),
                       sel ? 0.28f : (hov ? 0.14f : 0.09f), sel ? 0.42f : (hov ? 0.18f : 0.11f),
                       0.96f);
      const auto& fd = dalian::faction_at(static_cast<int>(i));
      char fline[128];
      std::snprintf(fline, sizeof(fline), "%s", fd.country);
      renderer.ui_text(fx + 8, ry + 4, 1.1f, fline, 0.9f, 0.92f, 0.94f, 1.f);
      if (clicked && hov) {
        player_faction_id = static_cast<int>(i);
        if (net.active()) net.set_faction(static_cast<std::uint16_t>(player_faction_id));
      }
    }

    // Kit column.
    const float kx = 320, ky = 132, kw = 260, kh = 40, kgap = 8;
    renderer.ui_text(kx, ky - 24, 1.6f, "SELECT KIT", 0.7f, 0.72f, 0.76f, 1.f);
    for (std::size_t i = 0; i < kits.size(); ++i) {
      const float y = ky + i * (kh + kgap);
      const bool sel = static_cast<int>(i) == selected_kit;
      const bool hov = rect_hit(mx, my, kx, y, kw, kh);
      renderer.ui_rect(kx, y, kw, kh, sel ? 0.16f : (hov ? 0.13f : 0.10f),
                       sel ? 0.28f : (hov ? 0.16f : 0.11f), sel ? 0.42f : (hov ? 0.20f : 0.13f),
                       0.96f);
      renderer.ui_rect(kx, y, 4, kh, sel ? 0.4f : 0.25f, sel ? 0.75f : 0.4f, 1.0f, 1.f);
      renderer.ui_text(kx + 16, y + 12, 1.7f, kits[i].name, 0.93f, 0.95f, 0.97f, 1.f);
      if (clicked && hov) selected_kit = static_cast<int>(i);
    }

    // Loadout summary.
    const KitDef& k = kits[selected_kit];
    const int wf = dalian::faction_kit_side(player_faction_id);
    const char* wn = weapon_defs[static_cast<std::size_t>(k.weapon[wf]) % weapon_defs.size()].name;
    const float sy = ky + kits.size() * (kh + kgap) + 14;
    char line[128];
    renderer.ui_text(kx, sy, 1.5f, "LOADOUT", 0.7f, 0.72f, 0.76f, 1.f);
    std::snprintf(line, sizeof(line), "Primary : %s", wn);
    renderer.ui_text(kx, sy + 24, 1.5f, line, 0.86f, 0.88f, 0.9f, 1.f);
    std::snprintf(line, sizeof(line), "Gadget  : %s", k.gadget);
    renderer.ui_text(kx, sy + 44, 1.5f, line, 0.86f, 0.88f, 0.9f, 1.f);
    renderer.ui_text(kx, sy + 64, 1.5f, "Sidearm : M9 Pistol", 0.7f, 0.72f, 0.75f, 1.f);

    // Spawn map.
    const float mpx = 610, mpy = 132, mpw = W - 650, mph = H - 250;
    renderer.ui_text(mpx, mpy - 24, 1.6f, "SELECT SPAWN POINT", 0.7f, 0.72f, 0.76f, 1.f);
    renderer.ui_rect(mpx, mpy, mpw, mph, 0.08f, 0.11f, 0.10f, 0.96f);
    renderer.ui_rect(mpx, mpy, mpw, 3, 0.3f, 0.5f, 0.6f, 1.f);
    renderer.ui_rect(mpx, mpy + mph - 3, mpw, 3, 0.3f, 0.5f, 0.6f, 1.f);
    glm::vec2 mn(1e30f, 1e30f), mx2(-1e30f, -1e30f);
    for (const auto& s : spawn_points) {
      mn.x = std::min(mn.x, s.pos.x);
      mn.y = std::min(mn.y, s.pos.z);
      mx2.x = std::max(mx2.x, s.pos.x);
      mx2.y = std::max(mx2.y, s.pos.z);
    }
    glm::vec2 span = mx2 - mn;
    if (span.x < 1.f) span.x = 1.f;
    if (span.y < 1.f) span.y = 1.f;
    const float pad = 44.f;
    auto to_map = [&](const glm::vec3& p) {
      const float u = (p.x - mn.x) / span.x, v = (p.z - mn.y) / span.y;
      return glm::vec2(mpx + pad + u * (mpw - 2 * pad), mpy + pad + v * (mph - 2 * pad));
    };
    for (std::size_t i = 0; i < spawn_points.size(); ++i) {
      const glm::vec2 c = to_map(spawn_points[i].pos);
      const bool sel = static_cast<int>(i) == selected_spawn;
      const float ms = sel ? 20.f : 15.f;
      const bool hov = rect_hit(mx, my, c.x - ms * 0.5f, c.y - ms * 0.5f, ms, ms);
      renderer.ui_rect(c.x - ms * 0.5f, c.y - ms * 0.5f, ms, ms, sel ? 0.2f : (hov ? 0.5f : 0.16f),
                       sel ? 0.85f : (hov ? 0.65f : 0.5f), sel ? 0.4f : (hov ? 0.5f : 0.32f), 1.f);
      renderer.ui_text(c.x + ms * 0.6f, c.y - 7, 1.3f, spawn_points[i].name.c_str(), 0.9f, 0.92f,
                       0.95f, 1.f);
      if (clicked && hov) selected_spawn = static_cast<int>(i);
    }

    // Deploy button.
    const float bw = 240, bh = 54, bx = W - bw - 40, by = H - bh - 36;
    const bool bhov = rect_hit(mx, my, bx, by, bw, bh);
    renderer.ui_rect(bx, by, bw, bh, bhov ? 0.15f : 0.1f, bhov ? 0.55f : 0.4f, bhov ? 0.25f : 0.16f,
                     1.f);
    const float tw = renderer.ui_text_width("DEPLOY", 2.6f);
    renderer.ui_text(bx + (bw - tw) * 0.5f, by + 16, 2.6f, "DEPLOY", 0.96f, 1.0f, 0.96f, 1.f);
    if (clicked && bhov) {
      apply_loadout();
      if (have_game_audio) game_audio.play_weapon_deploy(resources, !third_person);
      deploy_open = false;
    }
  };

  while (running) {
    bool launch_requested = false;  // set by the T key, serviced after camera update
    bool kamikaze_launch_requested = false;
    bool heli_flare_requested = false;  // set by X while piloting a helicopter
    deploy_click = false;
    air_stick_moved = false;
    // Free the cursor for menu interaction; capture it for mouse-look otherwise.
    const SDL_bool want_rel = (mouse_look && !deploy_open) ? SDL_TRUE : SDL_FALSE;
    if (SDL_GetRelativeMouseMode() != want_rel) SDL_SetRelativeMouseMode(want_rel);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        app_running = false;
        running = false;
      } else if (dalian::handle_display_hotkey(window, settings, e)) {
        dalian::sync_drawable_size(window, cur_w, cur_h);
        renderer.set_viewport(cur_w, cur_h);
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        if (deploy_open) {
          deploy_open = false;  // close deploy screen without quitting
        } else if (headless_shot) {
          running = false;  // headless capture may still abort on ESC
        } else {
          pause_open = true;  // in-game pause menu (Alt+F4 / window X to quit)
        }
      } else if (deploy_open && e.type == SDL_MOUSEWHEEL) {
        deploy_faction_scroll -= e.wheel.y * 24.f;
      } else if (deploy_open && e.type == SDL_MOUSEBUTTONDOWN &&
                 e.button.button == SDL_BUTTON_LEFT) {
        deploy_click = true;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN && !drone_mode &&
                 in_vehicle < 0) {
        deploy_open = !deploy_open;  // open/close the deploy (spawn) screen
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_TAB) {
        // Mouse flight is mandatory while piloting — TAB only toggles look on foot.
        const bool piloting =
            in_vehicle >= 0 && vehicles[in_vehicle].is_air && player_seat == 0;
        if (!piloting) {
          mouse_look = !mouse_look;
          SDL_SetRelativeMouseMode(mouse_look ? SDL_TRUE : SDL_FALSE);
        }
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_v && have_soldier) {
        third_person = !third_person;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_e && !drone_mode) {
        // Enter the nearest vehicle, or exit the current one (BF2-style).
        if (in_vehicle >= 0) {
          // Dismount: step out beside the vehicle onto whatever solid surface is
          // there. Using the vehicle's own height as the reference means you step
          // onto a carrier deck / bridge instead of dropping through it to the sea
          // floor below (find_safe_spawn probes the deck and clears hulls/walls).
          Vehicle& v = vehicles[in_vehicle];
          const float hd = glm::radians(v.heading);
          const glm::vec3 side(std::cos(hd), 0.f, -std::sin(hd));
          glm::vec3 want = v.pos + side * 4.0f;
          want.y = v.pos.y;  // reference height for the deck/surface probe
          const glm::vec3 safe = find_safe_spawn(want);
          player.position = {safe.x, safe.y + player.eye_height + 0.3f, safe.z};
          player.vertical_velocity = 0.f;
          // Level a helicopter's attitude on exit so a parked airframe doesn't
          // stay frozen mid-bank, and rebuild its transform level.
          if (v.is_air) {
            v.pitch = 0.f;
            v.roll = 0.f;
            v.vel = glm::vec3(0.f);
            const float hrad = glm::radians(v.heading);
            v.model = glm::rotate(glm::translate(glm::mat4(1.0f), v.pos), hrad, glm::vec3(0, 1, 0));
          }
          for (auto& s : v.seats) s.occupant = -1;  // crew leaves with the player
          // Resume the on-foot view facing the way the vehicle pointed, and clear
          // any leftover flight-stick deflection.
          yaw = 90.f - v.heading;
          air_pitch_stick = air_roll_stick = 0.f;
          in_vehicle = -1;
          player_seat = 0;
        } else {
          int best = -1;
          float best_d2 = 6.0f * 6.0f;  // enter radius (m)
          for (std::size_t i = 0; i < vehicles.size(); ++i) {
            // Only real vehicles (skip static props / stationary weapons).
            if (vehicles[i].mesh_key.find("vehicles/") == std::string::npos) continue;
            const glm::vec3 d(vehicles[i].pos.x - player.position.x, 0.f,
                              vehicles[i].pos.z - player.position.z);
            const float d2 = d.x * d.x + d.z * d.z;
            if (d2 < best_d2) {
              best_d2 = d2;
              best = static_cast<int>(i);
            }
          }
          if (best >= 0) {
            in_vehicle = best;
            player_seat = 0;  // you always board into the driver/pilot seat
            air_pitch_stick = air_roll_stick = 0.f;
            Vehicle& nv = vehicles[best];
            for (auto& s : nv.seats) s.occupant = -1;
            if (!nv.seats.empty()) nv.seats[0].occupant = 0;    // you drive/pilot
            if (nv.seats.size() > 1) nv.seats[1].occupant = 1;  // an AI mans the gun
            // BF2 captures the mouse for flight — relative look must stay on.
            if (nv.is_air) {
              mouse_look = true;
              SDL_SetRelativeMouseMode(SDL_TRUE);
              int dx = 0, dy = 0;
              SDL_GetRelativeMouseState(&dx, &dy);  // discard warp delta
              nv.pitch = 0.f;
              nv.roll = 0.f;
              nv.vel = glm::vec3(0.f);
              nv.throttle = 0.f;
              nv.wheels_on_ground = true;
              air_pitch_stick = air_roll_stick = 0.f;
              air_input_grace = 0.35f;
            }
          }
        }
      } else if (e.type == SDL_KEYDOWN && in_vehicle >= 0 && e.key.keysym.sym >= SDLK_F1 &&
                 e.key.keysym.sym <= SDLK_F8) {
        // BF2-style seat switch: F1..Fn select a crew seat. If a bot is there it
        // swaps into your vacated seat (so an AI can take over the controls).
        const int want = static_cast<int>(e.key.keysym.sym - SDLK_F1);
        Vehicle& v = vehicles[in_vehicle];
        if (want < static_cast<int>(v.seats.size()) && want != player_seat) {
          const int prev = v.seats[want].occupant;      // -1 empty, 1 AI
          v.seats[player_seat].occupant = prev == 1 ? 1 : -1;  // bot takes old seat
          v.seats[want].occupant = 0;                    // you take the new seat
          player_seat = want;
        }
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_f) {
        ballistic = !ballistic;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r && !drone_mode &&
                 in_vehicle < 0 && !deploy_open) {
        // Reload: pull rounds from the reserve into the magazine.
        if (!reloading && reserve_ammo > 0 && mag_ammo < kMagSize) {
          reloading = true;
          reload_timer = 2.0f;
          if (have_game_audio) {
            game_audio.play_weapon_reload(resources, !third_person);
            game_audio.play_voice(resources, voice_bank, "AUTO_MOODGP_reloading");
          }
        }
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_t && !drone_mode) {
        launch_requested = true;  // fire an AT missile (from a launcher/vehicle)
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_g && !drone_mode &&
                 in_vehicle < 0 && !deploy_open) {
        // Throw a frag grenade in an arc along the aim direction.
        if (grenades_left > 0) {
          const glm::vec3 eye0(player.position.x, player.position.y, player.position.z);
          glm::vec3 fr;
          fr.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
          fr.y = std::sin(glm::radians(pitch));
          fr.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));
          fr = glm::normalize(fr);
          live_grenades.push_back({eye0 + fr * 0.6f, fr * 22.f + glm::vec3(0, 3.f, 0), 3.0f});
          --grenades_left;
          if (have_game_audio)
            game_audio.play_voice(resources, voice_bank, "AUTO_MOODGP_throwingfraggrenade");
        }
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_c && !drone_mode &&
                 in_vehicle < 0 && !deploy_open) {
        // Place a C4 charge on the surface ahead (Spec Ops / Sniper claymore).
        if (c4_left > 0) {
          const glm::vec3 eye0(player.position.x, player.position.y, player.position.z);
          glm::vec3 fr;
          fr.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
          fr.y = std::sin(glm::radians(pitch));
          fr.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));
          fr = glm::normalize(fr);
          const auto h = world.raycast({eye0.x, eye0.y, eye0.z}, {fr.x, fr.y, fr.z}, 6.f);
          const glm::vec3 pt =
              h.hit ? glm::vec3(h.point.x, h.point.y, h.point.z) : eye0 + fr * 3.f;
          placed_c4.push_back(pt);
          --c4_left;
        }
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_x && !drone_mode &&
                 in_vehicle >= 0 && vehicles[in_vehicle].is_air) {
        heli_flare_requested = true;  // punch out a countermeasure burst
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_x && !drone_mode &&
                 in_vehicle < 0 && !deploy_open) {
        // Detonate all placed C4.
        for (const auto& pt : placed_c4) {
          explode_at(pt, 7.f, 160.f);
          explosions.push_back({pt, 0.f, 0.55f});
          for (int s = 0; s < 6; ++s) smoke.push_back({pt, 0.f, 1.4f});
        }
        placed_c4.clear();
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_h && !drone_mode &&
                 in_vehicle < 0 && !deploy_open) {
        // Medic self-heal.
        if (has_medkit && medkit_cd <= 0.f && player_health < 100.f) {
          player_health = std::min(100.f, player_health + 45.f);
          medkit_cd = 6.f;
        }
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_n && !drone_mode) {
        kamikaze_launch_requested = true;
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_b) {
        // Launch or recall the FPV recon drone (not available during kamikaze).
        if (kamikaze_mode) {
          // No recall on a loitering munition.
        } else {
          drone_mode = !drone_mode;
          if (drone_mode) {
            drone = bf2::DroneController{};
            const glm::vec3 launch(player.position.x, player.position.y + 0.6f,
                                   player.position.z);
            drone.position = launch;
            drone_prev_pos = launch;
            drone_throttle = 0.30f;  // near hover
            signal = 1.f;
          }
        }
      } else if (e.type == SDL_MOUSEMOTION && mouse_look && drone_mode && !deploy_open) {
        drone_stick_roll += e.motion.xrel * 0.020f;
        drone_stick_pitch += e.motion.yrel * 0.020f;
      } else if (!drone_mode && !deploy_open &&
                 ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q) ||
                  e.type == SDL_MOUSEWHEEL)) {
        // Cycle to the next weapon that loads successfully (Q or mouse wheel).
        const int dir = (e.type == SDL_MOUSEWHEEL && e.wheel.y < 0) ? -1 : 1;
        for (std::size_t step = 0; step < weapon_defs.size(); ++step) {
          weapon_index = (weapon_index + weapon_defs.size() + dir) % weapon_defs.size();
          if (load_weapon(weapon_index)) break;
        }
        bind_weapon_audio();
      } else if (e.type == SDL_MOUSEMOTION && mouse_look && !deploy_open && !pause_open &&
                 air_input_grace <= 0.f && in_vehicle >= 0 &&
                 vehicles[in_vehicle].is_air && player_seat == 0) {
        // BF2 aircraft mouse:
        //   Jet:  pull BACK (mouse down) = nose UP / pitch; mouse left/right = bank (air only).
        //   Heli: mouse = cyclic tilt (pitch + roll).
        air_stick_moved = true;
        const Vehicle& av = vehicles[in_vehicle];
        const float inv = air_invert_fn() ? -1.f : 1.f;
        const float sens_scale = sensitivity / 0.12f;
        const float pitch_sens = (av.is_heli ? 0.020f : 0.032f) * sens_scale;
        const float roll_sens = (av.is_heli ? 0.016f : 0.018f) * sens_scale;
        // Pull back on stick (mouse toward you / cursor down) = positive pitch input.
        air_pitch_stick =
            std::clamp(air_pitch_stick + e.motion.yrel * pitch_sens * inv, -1.f, 1.f);
        if (av.is_heli) {
          air_roll_stick = std::clamp(air_roll_stick + e.motion.xrel * roll_sens, -1.f, 1.f);
        } else if (!av.wheels_on_ground) {
          // Bank: mouse left = bank left (negative roll rate).
          air_roll_stick =
              std::clamp(air_roll_stick - e.motion.xrel * roll_sens, -1.f, 1.f);
        }
      } else if (e.type == SDL_MOUSEMOTION && mouse_look && !deploy_open && !pause_open) {
        yaw += e.motion.xrel * sensitivity;
        pitch -= e.motion.yrel * sensitivity;
        pitch = std::clamp(pitch, -89.f, 89.f);
      } else if (e.type == SDL_WINDOWEVENT &&
                 (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                  e.window.event == SDL_WINDOWEVENT_RESIZED ||
                  e.window.event == SDL_WINDOWEVENT_RESTORED ||
                  e.window.event == SDL_WINDOWEVENT_MAXIMIZED)) {
        dalian::sync_drawable_size(window, cur_w, cur_h);
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
    float dt = static_cast<float>(now - prev) / static_cast<float>(SDL_GetPerformanceFrequency());
    prev = now;
    dt = std::clamp(dt, 0.f, 0.05f);
    air_input_grace = std::max(0.f, air_input_grace - dt);
    if (in_vehicle >= 0 && vehicles[in_vehicle].is_air && player_seat == 0 &&
        air_input_grace <= 0.f && !air_stick_moved) {
      const float decay = std::exp(-5.f * dt);
      air_pitch_stick *= decay;
      air_roll_stick *= decay;
    }

    // Pump the network early so this frame renders the freshest remote states.
    // `net_fired` / `net_fire_*` capture a shot taken this frame for replication.
    if (net.active()) net.poll(dt);
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

    // Rotor spool: aircraft you're piloting wind up to full RPM over a few
    // seconds; parked/exited ones wind back down. Rotor blade angle accumulates
    // at a rate proportional to RPM, so a parked heli's blades sit still.
    {
      const float fdt = dt > 0.f ? dt : 1.f / 60.f;
      for (std::size_t i = 0; i < vehicles.size(); ++i) {
        Vehicle& av = vehicles[i];
        if (!av.is_air) continue;
        const float target = (static_cast<int>(i) == in_vehicle) ? 1.f : 0.f;
        const float rate = target > av.rotor_rpm ? 1.f / 6.f : 1.f / 4.f;  // 6s up, 4s down
        av.rotor_rpm = glm::clamp(av.rotor_rpm + glm::clamp(target - av.rotor_rpm, -rate * fdt,
                                                            rate * fdt),
                                  0.f, 1.f);
        av.rotor_spin += fdt * 62.f * av.rotor_rpm;  // spin scales with RPM
      }
    }
    // Ground-vehicle wheel spin (roll angle from road speed).
    {
      const float fdt = dt > 0.f ? dt : 1.f / 60.f;
      for (auto& gv : vehicles) {
        if (gv.is_air || gv.wheels.empty()) continue;
        const float rate = gv.speed / 0.32f;
        for (float& ws : gv.wheel_spin) ws += rate * fdt;
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
    } else if (in_vehicle >= 0) {
      // ---- Vehicle driving ----------------------------------------------
      Vehicle& v = vehicles[in_vehicle];
      const float step = dt > 0.f ? dt : 1.f / 60.f;
      const bool boost = keys != nullptr && keys[SDL_SCANCODE_LSHIFT];
      // Move the vehicle by `delta`, but crash-stop against solid obstacles
      // (building walls, etc). We cast along the travel direction from bumper
      // height and only block on near-vertical surfaces (|normal.y| small) so
      // that terrain slopes and ramps don't falsely stop the vehicle. Returns
      // true if it hit something (so the caller can kill momentum).
      const float veh_radius = v.is_air ? 3.0f : 2.4f;
      auto move_vehicle = [&](const glm::vec3& delta) -> bool {
        const float dist = glm::length(glm::vec3(delta.x, 0.f, delta.z));
        if (dist < 1e-4f) {
          v.pos += delta;
          return false;
        }
        const glm::vec3 d = glm::normalize(glm::vec3(delta.x, 0.f, delta.z));
        const glm::vec3 o(v.pos.x, v.pos.y + 1.2f, v.pos.z);
        const auto hit = world.raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, dist + veh_radius);
        if (hit.hit && hit.distance < dist + veh_radius && std::fabs(hit.normal.y) < 0.6f) {
          // Stop just short of the wall; slide is intentionally disabled so it
          // reads as a real crash rather than a smooth glide.
          const float back = std::max(0.f, hit.distance - veh_radius);
          v.pos += d * back;
          return true;
        }
        v.pos += delta;
        return false;
      };
      // Landing surface for aircraft: the terrain, or a solid deck below the
      // aircraft (carrier flight deck / helipad on a structure). Lets aircraft
      // take off from and set down on the carrier instead of sinking to the sea.
      auto air_floor = [&]() -> float {
        float f = world.terrain_height(v.pos.x, v.pos.z);
        const auto dn = world.raycast({v.pos.x, v.pos.y + 4.f, v.pos.z}, {0.f, -1.f, 0.f}, 220.f);
        if (dn.hit && std::fabs(dn.normal.y) > 0.4f && dn.point.y > f + 1.0f &&
            dn.point.y <= v.pos.y + 4.f) {
          f = dn.point.y;
        }
        return f + v.land_clearance;
      };
      const bool driving = player_seat == 0;  // only the pilot/driver flies it
      if (v.is_air && v.is_heli) {
        // ---- BF2 helicopter flight (matches the retail control scheme) ----
        //   W / S       collective: increase (W) / decrease (S) motor — up/down
        //   A / D       yaw (tail rotor) — turn left / right
        //   Mouse X     roll/slide (bank); Mouse Y cyclic pitch (fwd/back tilt)
        //   Shift       extra power (boost)
        // The cyclic is an ATTITUDE command that holds where you leave it, so you
        // actively fly it level (there's no auto-centre) — pure BF2 feel.
        // Off the pilot seat an autopilot holds a stable hover so you can shoot.
        float coll_in = 0.f, yaw_in = 0.f;
        float pitch_cmd = 0.f, roll_cmd = 0.f;
        // Deeper nose-over range so you can really tip forward and accelerate; roll
        // is a gentler range — BF2 strafe uses only SLIGHT mouse left/right.
        const float max_pitch = 42.f, max_roll = 28.f;
        if (driving && keys != nullptr) {
          if (keys[SDL_SCANCODE_W]) coll_in += 1.f;  // climb
          if (keys[SDL_SCANCODE_S]) coll_in -= 1.f;  // descend
          if (keys[SDL_SCANCODE_SPACE]) coll_in += 1.f;  // fine climb (alt)
          if (keys[SDL_SCANCODE_LCTRL]) coll_in -= 1.f;  // fine descend (alt)
          if (keys[SDL_SCANCODE_A]) yaw_in += 1.f;   // yaw left
          if (keys[SDL_SCANCODE_D]) yaw_in -= 1.f;   // yaw right
          // Cyclic tilt from the flight stick: mouse forward (stick < 0) tips the
          // nose down to fly forward. Roll is INVERTED here so mouse-left banks
          // left / mouse-right banks right (matching how BF2 reads), and the
          // gentler max makes the bank progressive across the mouse range.
          pitch_cmd = air_pitch_stick * max_pitch;
          roll_cmd = -air_roll_stick * max_roll;
        }
        // Rotor must be spun up before it can fly; all authority scales with RPM,
        // so a freshly-entered heli spools up for a few seconds before lifting.
        const float authority = v.rotor_rpm;
        auto approach = [&](float cur, float cmd, float rate) {
          return cur + (cmd - cur) * std::min(1.f, rate * step);
        };
        v.pitch = approach(v.pitch, pitch_cmd * authority, 3.4f);
        v.roll = approach(v.roll, roll_cmd * authority, 2.8f);
        // Yaw the aircraft from the pedals (turns while hovering, too).
        v.heading += yaw_in * 48.f * step * authority;
        const float hd = glm::radians(v.heading);
        const glm::vec3 fwd(std::sin(hd), 0.f, std::cos(hd));
        const glm::vec3 rgt(std::cos(hd), 0.f, -std::sin(hd));  // aircraft right
        // Horizontal thrust from the cyclic: nose-down (pitch<0) pushes forward,
        // and banking (roll) slides the chopper sideways — the BF2 "strafe".
        const float pwr = (boost ? 46.f : 32.f) * authority;
        v.vel += fwd * (-std::sin(glm::radians(v.pitch)) * pwr) * step;
        v.vel += rgt * (std::sin(glm::radians(v.roll)) * pwr * 0.7f) * step;
        // Collective lift vs gravity: neutral collective at full RPM holds a
        // hover; W adds lift to climb, S removes it to descend. When nobody is
        // flying, the autopilot bleeds off any climb/sink to hold altitude.
        float lift;
        if (driving) {
          lift = (9.81f + coll_in * 15.f) * authority;
        } else {
          lift = (9.81f - v.vel.y * 2.0f) * authority;  // hover hold
          v.pitch = approach(v.pitch, 0.f, 2.0f);       // autopilot levels out
          v.roll = approach(v.roll, 0.f, 2.0f);
        }
        v.vel.y += (lift - 9.81f) * step;
        // Aerodynamic drag (stronger horizontally) bleeds momentum.
        v.vel.x -= v.vel.x * 0.9f * step;
        v.vel.z -= v.vel.z * 0.9f * step;
        v.vel.y -= v.vel.y * 1.4f * step;
        v.vel = glm::clamp(v.vel, glm::vec3(-95.f), glm::vec3(95.f));
        // Integrate horizontally (with building collision), then vertically.
        if (move_vehicle(glm::vec3(v.vel.x, 0.f, v.vel.z) * step)) {
          v.vel.x *= 0.1f;
          v.vel.z *= 0.1f;
        }
        v.pos.y += v.vel.y * step;
        // Land on terrain or a deck below, using the true skid height.
        const float floor_y = air_floor();
        if (v.pos.y < floor_y) {
          v.pos.y = floor_y;
          if (v.vel.y < 0.f) v.vel.y = 0.f;
          // Settle to level once the skids are on the ground.
          v.pitch = approach(v.pitch, 0.f, 4.0f);
          v.roll = approach(v.roll, 0.f, 4.0f);
        }
        // Diagnostic (BF2_HELIDEMO): pin an airborne bank so a headless capture
        // shows the pitch/roll attitude model. No effect in normal play.
        if (std::getenv("BF2_HELIDEMO")) {
          v.rotor_rpm = 1.f;
          v.pitch = -18.f;
          v.roll = 28.f;
          v.pos.y = world.terrain_height(v.pos.x, v.pos.z) + 25.f;
        }
      } else if (v.is_air) {
        // ---- BF2 fixed-wing jet ------------------------------------------------
        // Ground: W throttle, A/D rudder, pull BACK on mouse to rotate once rolling.
        // Air: mouse pitch/roll; bank to turn. Stick springs to center when released.
        auto approach = [&](float cur, float cmd, float rate) {
          return cur + (cmd - cur) * std::min(1.f, rate * step);
        };
        constexpr float kTakeoff = 28.f;    // ~100 km/h
        constexpr float kRotateMin = 12.f;  // start reading rotation input

        float thr_in = 0.f, yaw_in = 0.f;
        if (driving && keys != nullptr) {
          if (keys[SDL_SCANCODE_W]) thr_in += 1.f;
          if (keys[SDL_SCANCODE_S]) thr_in -= 1.f;
          if (keys[SDL_SCANCODE_A]) yaw_in -= 1.f;
          if (keys[SDL_SCANCODE_D]) yaw_in += 1.f;
        }
        v.throttle = glm::clamp(v.throttle + thr_in * step * 0.55f, 0.f, 1.f);
        if (thr_in <= 0.f) v.throttle = glm::max(0.f, v.throttle - step * 0.05f);

        const float floor_y = air_floor();
        const float hd = glm::radians(v.heading);
        const glm::vec3 flat_fwd(std::sin(hd), 0.f, std::cos(hd));
        float fwd_spd = glm::dot(glm::vec3(v.vel.x, 0.f, v.vel.z), flat_fwd);
        const bool on_ground = v.pos.y <= floor_y + 0.22f && v.vel.y <= 1.2f;
        v.wheels_on_ground = on_ground;

        if (on_ground) {
          v.pos.y = floor_y;
          v.vel.y = 0.f;
          v.roll = approach(v.roll, 0.f, 14.f);
          air_roll_stick = approach(air_roll_stick, 0.f, 12.f);

          const float thrust = (boost ? 38.f : 26.f) * v.throttle;
          if (v.throttle > 0.04f && thr_in >= 0.f)
            fwd_spd += thrust * step;
          else
            fwd_spd -= (thr_in < 0.f ? 34.f : 11.f) * step *
                       glm::max(0.15f, std::fabs(fwd_spd) / 15.f);
          fwd_spd = glm::clamp(fwd_spd, -5.f, boost ? 58.f : 48.f);
          v.vel = flat_fwd * fwd_spd;

          const float steer = 42.f * glm::clamp(std::fabs(fwd_spd) / 10.f, 0.08f, 1.f);
          v.heading += yaw_in * steer * step;

          // Pull back (positive pitch stick) to rotate nose up once rolling.
          if (fwd_spd >= kRotateMin && air_pitch_stick > 0.04f) {
            const float t = glm::clamp((fwd_spd - kRotateMin) / (kTakeoff - kRotateMin), 0.f, 1.f);
            v.pitch = approach(v.pitch, air_pitch_stick * 24.f * t, 6.f);
          } else {
            v.pitch = approach(v.pitch, 0.f, 12.f);
          }

          if (move_vehicle(flat_fwd * fwd_spd * step)) fwd_spd *= 0.15f;
          v.vel = flat_fwd * fwd_spd;

          if (fwd_spd >= kTakeoff && v.pitch >= 5.f) {
            v.vel = flat_fwd * fwd_spd;
            v.vel.y = std::sin(glm::radians(v.pitch)) * fwd_spd * 0.32f;
            v.pos.y += v.vel.y * step;
            if (v.pos.y <= floor_y + 0.22f) {
              v.pos.y = floor_y;
              v.vel.y = 0.f;
            } else {
              v.wheels_on_ground = false;
            }
          }
        } else {
          v.wheels_on_ground = false;
          float spd = glm::length(v.vel);
          const float ctrl = glm::clamp(spd / 30.f, 0.4f, 1.f);

          // Pitch: pull back = nose up. Roll: mouse left = bank left.
          v.pitch += air_pitch_stick * 55.f * ctrl * step;
          v.roll -= air_roll_stick * 50.f * ctrl * step;
          v.pitch = glm::clamp(v.pitch, -55.f, 55.f);
          v.roll = glm::clamp(v.roll, -75.f, 75.f);

          const float bank_turn =
              std::sin(glm::radians(v.roll)) * 52.f * glm::clamp(spd / 26.f, 0.f, 1.f);
          v.heading += (bank_turn + yaw_in * 22.f) * step;

          const float pr = glm::radians(v.pitch);
          const glm::vec3 nose(std::sin(hd) * std::cos(pr), std::sin(pr),
                               std::cos(hd) * std::cos(pr));
          const float max_thrust = boost ? 52.f : 34.f;
          v.vel += nose * (v.throttle * max_thrust) * step;

          spd = glm::length(v.vel);
          if (spd > 24.f)
            v.vel = glm::mix(v.vel, nose * spd, std::min(1.f, 1.1f * step));
          v.vel -= v.vel * (0.08f + 0.0025f * spd) * step;

          const float lift = glm::clamp(spd / kTakeoff, 0.f, 1.f);
          v.vel.y -= 9.81f * (1.f - lift) * step;
          v.vel = glm::clamp(v.vel, glm::vec3(-200.f), glm::vec3(200.f));

          if (move_vehicle(glm::vec3(v.vel.x, 0.f, v.vel.z) * step)) {
            v.vel.x *= 0.12f;
            v.vel.z *= 0.12f;
          }
          v.pos.y += v.vel.y * step;

          if (v.pos.y <= floor_y + 0.22f && v.vel.y <= 0.f) {
            v.pos.y = floor_y;
            v.vel.y = 0.f;
            v.wheels_on_ground = true;
            v.roll = approach(v.roll, 0.f, 8.f);
            if (spd < kTakeoff) v.pitch = approach(v.pitch, 0.f, 6.f);
          }
        }
        if (std::getenv("BF2_JETDEMO")) {
          v.throttle = 1.f;
          v.pitch = 12.f;
          v.roll = 18.f;
          v.pos.y = world.terrain_height(v.pos.x, v.pos.z) + 40.f;
        }
      } else {
        // Ground vehicle: throttle + steer, glued to the terrain.
        const float accel = 18.f, max_spd = boost ? 30.f : 20.f, brake = 34.f;
        bool throttling = false;
        if (driving && keys != nullptr) {
          if (keys[SDL_SCANCODE_W]) {
            v.speed += accel * step;
            throttling = true;
          }
          if (keys[SDL_SCANCODE_S]) {
            v.speed -= accel * step;
            throttling = true;
          }
          // Steering scales with speed and reverses when backing up.
          const float turn = 75.f * step * std::clamp(std::fabs(v.speed) / 8.f, 0.15f, 1.f);
          const float dir = v.speed >= 0.f ? 1.f : -1.f;
          if (keys[SDL_SCANCODE_A]) v.heading += turn * dir;
          if (keys[SDL_SCANCODE_D]) v.heading -= turn * dir;
          float steer_target = 0.f;
          if (keys[SDL_SCANCODE_A]) steer_target = 0.42f;
          if (keys[SDL_SCANCODE_D]) steer_target = -0.42f;
          v.visual_steer = glm::mix(v.visual_steer, steer_target, 1.f - std::exp(-10.f * step));
        }
        if (!throttling) {  // engine braking / rolling resistance
          const float d = brake * 0.4f * step;
          v.speed = std::fabs(v.speed) <= d ? 0.f : v.speed - d * (v.speed > 0 ? 1.f : -1.f);
        }
        v.speed = std::clamp(v.speed, -max_spd * 0.5f, max_spd);
        const float hd = glm::radians(v.heading);
        const glm::vec3 fwd(std::sin(hd), 0.f, std::cos(hd));
        // Crash into buildings instead of clipping through them.
        if (move_vehicle(fwd * v.speed * step)) {
          const float impact = std::fabs(v.speed);
          v.speed = 0.f;
          if (impact > 12.f) player_health -= (impact - 12.f) * 3.f;  // hard crash hurts
        }
        // Rest on whatever solid surface is directly beneath the chassis, not
        // just the heightmap: casting down catches bridge decks, road props and
        // rail beds laid over the terrain, so the vehicle drives across them
        // instead of sinking to the ground below.
        auto ground_at = [&](float x, float z) -> float {
          const float terr = world.terrain_height(x, z);
          float s = terr;
          const float top = std::max(v.pos.y, terr) + 2.0f;
          const auto dn = world.raycast({x, top, z}, {0.f, -1.f, 0.f}, 12.f);
          if (dn.hit && dn.normal.y > 0.5f) s = std::max(s, top - dn.distance);
          return s;
        };
        const glm::vec3 fdir(std::sin(hd), 0.f, std::cos(hd));
        const glm::vec3 sdir(fdir.z, 0.f, -fdir.x);  // right
        const float L = 2.4f, Wd = 1.3f;             // approx half wheelbase / track
        const float hF = ground_at(v.pos.x + fdir.x * L, v.pos.z + fdir.z * L);
        const float hB = ground_at(v.pos.x - fdir.x * L, v.pos.z - fdir.z * L);
        const float hR = ground_at(v.pos.x + sdir.x * Wd, v.pos.z + sdir.z * Wd);
        const float hL = ground_at(v.pos.x - sdir.x * Wd, v.pos.z - sdir.z * Wd);
        v.pos.y = ground_at(v.pos.x, v.pos.z) + v.clearance;
        // Chassis normal from the four wheel-contact heights (spans the wheelbase
        // so it rides smoothly over bumps, like real suspension).
        const float dFwd = (hF - hB) / (2.f * L);
        const float dSide = (hR - hL) / (2.f * Wd);
        const glm::vec3 target_n =
            glm::normalize(glm::vec3(0.f, 1.f, 0.f) - fdir * dFwd - sdir * dSide);
        const float k = 1.f - std::exp(-8.f * step);  // suspension response
        v.ground_normal = glm::normalize(glm::mix(v.ground_normal, target_n, k));
      }
      // Rebuild the body transform. Aircraft bank/pitch with their flight
      // attitude; ground vehicles tilt so the chassis up-axis follows the
      // (smoothed) ground normal. Parts follow via v.model * part.local.
      {
        const float hrad = glm::radians(v.heading);
        if (v.is_air) {
          glm::mat4 m = glm::translate(glm::mat4(1.0f), v.pos);
          m = glm::rotate(m, hrad, glm::vec3(0, 1, 0));                   // yaw
          m = glm::rotate(m, glm::radians(-v.pitch), glm::vec3(1, 0, 0));  // pitch (nose down = fwd)
          m = glm::rotate(m, glm::radians(-v.roll), glm::vec3(0, 0, 1));   // bank
          v.model = m;
        } else {
          const glm::vec3 up = v.ground_normal;
          const glm::vec3 fdir(std::sin(hrad), 0.f, std::cos(hrad));
          const glm::vec3 rgt = glm::normalize(glm::cross(up, fdir));
          const glm::vec3 fwd2 = glm::normalize(glm::cross(rgt, up));
          glm::mat4 R(1.0f);
          R[0] = glm::vec4(rgt, 0.f);
          R[1] = glm::vec4(up, 0.f);
          R[2] = glm::vec4(fwd2, 0.f);
          v.model = glm::translate(glm::mat4(1.0f), v.pos) * R;
        }
      }
      // Keep the soldier "with" the vehicle so re-exit / HUD stays sane.
      player.position = {v.pos.x, v.pos.y + player.eye_height, v.pos.z};
      player.vertical_velocity = 0.f;
    } else if (keys != nullptr && !deploy_open) {
      glm::vec3 move(0.f);
      if (keys[SDL_SCANCODE_W]) move += flat_front;
      if (keys[SDL_SCANCODE_S]) move -= flat_front;
      if (keys[SDL_SCANCODE_D]) move += right;
      if (keys[SDL_SCANCODE_A]) move -= right;
      const bool wants_move = glm::length(move) > 0.001f;
      // Sprint drains stamina; when exhausted you drop back to a jog and must
      // recover before sprinting again.
      const bool wants_sprint = keys[SDL_SCANCODE_LSHIFT] && wants_move;
      const bool can_sprint = wants_sprint && player_stamina > 5.f;
      if (can_sprint) {
        player_stamina = std::max(0.f, player_stamina - 22.f * dt);
      } else {
        player_stamina = std::min(100.f, player_stamina + (wants_move ? 8.f : 16.f) * dt);
      }
      const float speed = can_sprint ? 12.f : 6.5f;
      if (wants_move) {
        move = glm::normalize(move) * speed;
      }
      player.desired_velocity = {move.x, 0.f, move.z};
      if (keys[SDL_SCANCODE_SPACE] && player.on_ground) {
        player.vertical_velocity = 6.5f;
      }
      world.step_character(player, dt > 0.f ? dt : 1.f / 60.f);
      // Solid vehicles: push the walker out of any hull it walked into.
      {
        float px = player.position.x, pz = player.position.z;
        if (push_out_of_vehicles(px, pz, player.position.y - player.eye_height, in_vehicle)) {
          player.position.x = px;
          player.position.z = pz;
        }
      }
    } else {
      player.desired_velocity = {0.f, 0.f, 0.f};
      world.step_character(player, dt > 0.f ? dt : 1.f / 60.f);
    }

    const glm::vec3 eye(player.position.x, player.position.y, player.position.z);

    // Shooting: automatic fire while left mouse is held.
    voice_cooldown = std::max(0.f, voice_cooldown - dt);
    fire_cooldown = std::max(0.f, fire_cooldown - dt);
    muzzle_flash = std::max(0.f, muzzle_flash - dt);
    recoil = std::max(0.f, recoil - dt * 6.f);
    // Reload / gadget cooldowns.
    reload_timer = std::max(0.f, reload_timer - dt);
    medkit_cd = std::max(0.f, medkit_cd - dt);
    if (reloading && reload_timer <= 0.f) {
      const int need = kMagSize - mag_ammo;
      const int take = std::min(need, reserve_ammo);
      mag_ammo += take;
      reserve_ammo -= take;
      reloading = false;
    }

    const Uint32 mouse = SDL_GetMouseState(nullptr, nullptr);
    const bool lmb = (mouse & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    const bool rmb = (mouse & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    if (lmb && mouse_look && !drone_mode && in_vehicle < 0 && !deploy_open && !reloading &&
        mag_ammo <= 0 && voice_cooldown <= 0.f) {
      if (have_game_audio) {
        game_audio.play_voice(resources, voice_bank, "out_of_ammo");
        voice_cooldown = 2.5f;
      }
    }
    if (lmb && mouse_look && !drone_mode && in_vehicle < 0 && !deploy_open && !reloading &&
        mag_ammo > 0 && fire_cooldown <= 0.f) {
      --mag_ammo;
      // Auto-reload once the magazine runs dry.
      if (mag_ammo == 0 && reserve_ammo > 0) {
        reloading = true;
        reload_timer = 2.0f;
        if (have_game_audio) {
          game_audio.play_weapon_reload(resources, !third_person);
          game_audio.play_voice(resources, voice_bank, "AUTO_MOODGP_reloading");
        }
      }
      fire_cooldown = 0.1f;  // ~600 rpm
      muzzle_flash = 0.045f;
      recoil = std::min(1.f, recoil + 0.6f);
      if (have_game_audio) game_audio.play_weapon_fire(resources, !third_person);
      const glm::vec3 muzzle = eye + front * 0.6f + right * 0.18f - glm::vec3(0, 1, 0) * 0.12f;
      // Replicate this shot to other players (tracer + impact on their clients).
      net_fired = true;
      net_fire_o = muzzle;
      net_fire_d = front;
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

    // ---- Helicopter weapon systems -----------------------------------------
    heli_rocket_cd = std::max(0.f, heli_rocket_cd - dt);
    heli_gun_cd = std::max(0.f, heli_gun_cd - dt);
    heli_grocket_cd = std::max(0.f, heli_grocket_cd - dt);
    heli_flare_cd = std::max(0.f, heli_flare_cd - dt);
    if (in_vehicle >= 0 && vehicles[in_vehicle].is_air) {
      Vehicle& hv = vehicles[in_vehicle];
      const float hrad = glm::radians(hv.heading);
      const float prad = glm::radians(hv.pitch);
      // Aircraft nose direction (yaw + cyclic pitch): rockets fire along this.
      const glm::vec3 nose = glm::normalize(glm::vec3(std::sin(hrad) * std::cos(prad),
                                                      std::sin(prad),
                                                      std::cos(hrad) * std::cos(prad)));
      const glm::vec3 rightv(std::cos(hrad), 0.f, -std::sin(hrad));

      // Fire an unguided rocket from `origin` along `dir`, reusing the missile
      // flight model (no guidance, short-lived, high boost) + launch effects.
      auto fire_rocket = [&](const glm::vec3& origin, const glm::vec3& dir) {
        ActiveMissile am;
        am.m.guided = false;
        am.m.has_target = false;
        am.m.position = origin;
        am.m.velocity = dir * 60.f;
        am.m.boost_accel = 190.f;
        am.m.boost_time = 1.1f;
        am.m.max_speed = 240.f;
        am.m.turn_rate = 0.f;
        am.m.life = 6.f;
        am.m.gravity = 3.0f;  // rockets barely drop over their short flight
        am.m.mass = 8.f;
        am.prev_pos = origin;
        missiles.push_back(am);
        explosions.push_back({origin, 0.f, 0.15f});
        smoke.push_back({origin, 0.f, 0.9f});
      };

      const bool is_pilot = player_seat == 0;
      // Pilot: LMB ripple-fires the forward rocket pods (alternating L/R tubes).
      if (is_pilot && lmb && mouse_look && !deploy_open && heli_rocket_cd <= 0.f) {
        heli_rocket_cd = 0.28f;
        static int pod_side = 0;
        const glm::vec3 pod = hv.pos + nose * 3.0f - glm::vec3(0.f, 0.4f, 0.f) +
                              rightv * (pod_side++ % 2 == 0 ? 1.1f : -1.1f);
        fire_rocket(pod, nose);
      }

      // Flares (X): countermeasure burst that also decoys guided threats nearby.
      if (heli_flare_requested && heli_flare_cd <= 0.f) {
        heli_flare_cd = 3.0f;
        for (int i = 0; i < 10; ++i) {
          const float a = (static_cast<float>(i) / 10.f) * 6.2832f;
          const glm::vec3 vv(std::cos(a) * 6.f, -2.f - (i % 3) * 2.f, std::sin(a) * 6.f);
          flares.push_back({hv.pos - glm::vec3(0.f, 0.6f, 0.f), vv, 2.4f});
        }
        for (auto& am : missiles) {
          if (am.m.alive && am.m.guided && glm::distance(am.m.position, hv.pos) < 140.f) {
            am.m.guided = false;  // seeker distracted by the flare bloom
            am.homing_enemy = -1;
          }
        }
      }

      // ---- Gunner station (chin cannon + rockets) ---------------------------
      // The gunner seat is manned either by an AI crewman (when the player is
      // flying) or by the player (after switching seats with F2). The gunner
      // only shoots at an enemy it can actually see, within a forward arc, and
      // only after briefly tracking it — no more constant chatter at nothing.
      const int gunner_seat = 1;
      const bool has_gunner = hv.seats.size() > gunner_seat;
      const bool player_gunning = has_gunner && player_seat == gunner_seat;
      const bool ai_gunning =
          has_gunner && hv.seats[gunner_seat].occupant == 1 && player_seat != gunner_seat;
      gunner_engaging = false;
      if (has_gunner && (ai_gunning || player_gunning)) {
        const glm::vec3 gun = hv.pos + glm::vec3(0.f, -0.6f, 0.f) + nose * 2.0f;
        const glm::vec3 nose_flat = glm::normalize(glm::vec3(nose.x, 0.f, nose.z));

        // Acquire/keep a visible target within range and a forward arc.
        auto visible = [&](int i, float& out_range) -> bool {
          if (i < 0 || i >= static_cast<int>(enemies.size()) || !enemies[i].alive) return false;
          const glm::vec3 ep(enemies[i].pos.x, enemies[i].pos.y + 1.0f, enemies[i].pos.z);
          const glm::vec3 to = ep - gun;
          const float d = glm::length(to);
          if (d < 1e-3f || d > 200.f) return false;  // sensible engagement range
          const glm::vec3 dir = to / d;
          const glm::vec3 dir_flat = glm::normalize(glm::vec3(dir.x, 0.f, dir.z));
          if (glm::dot(dir_flat, nose_flat) < 0.30f) return false;  // ~72° forward arc
          const auto h = world.raycast({gun.x, gun.y, gun.z}, {dir.x, dir.y, dir.z}, d - 1.5f);
          if (h.hit) return false;  // no line of sight
          out_range = d;
          return true;
        };

        int tgt = -1;
        float range = 0.f;
        if (ai_gunning) {
          // Keep the current target if still valid; otherwise pick the closest.
          float cur_range = 0.f;
          if (gunner_target >= 0 && visible(gunner_target, cur_range)) {
            tgt = gunner_target;
            range = cur_range;
          } else {
            float best = 1e30f;
            for (std::size_t i = 0; i < enemies.size(); ++i) {
              float r = 0.f;
              if (visible(static_cast<int>(i), r) && r < best) {
                best = r;
                tgt = static_cast<int>(i);
                range = r;
              }
            }
          }
          // Track for a short beat before opening fire (no snap-firing).
          if (tgt >= 0 && tgt == gunner_target) {
            gunner_acquire += dt;
          } else {
            gunner_acquire = 0.f;
          }
          gunner_target = tgt;
        }

        const bool ai_ready = ai_gunning && tgt >= 0 && gunner_acquire > 0.4f;
        // The player-gunner fires on LMB along the aim; the AI fires when locked.
        if (player_gunning) {
          const glm::vec3 aim = front;  // player aims with the mouse
          if (lmb && mouse_look && !deploy_open && heli_gun_cd <= 0.f) {
            heli_gun_cd = 0.09f;
            gunner_engaging = true;
            const glm::vec3 sd = glm::normalize(
                aim + glm::vec3((std::rand() % 100 - 50) / 1100.f, (std::rand() % 100 - 50) / 1100.f,
                                (std::rand() % 100 - 50) / 1100.f));
            const auto eh = shoot_enemies(gun, sd, 260.f);
            if (eh.idx >= 0) {
              damage_enemy(eh.idx, eh.zone);
              tracers.push_back({gun, eh.point, 0.05f});
              impacts.push_back({eh.point, 0.35f});
            } else {
              tracers.push_back({gun, gun + sd * 260.f, 0.05f});
            }
          }
          if (rmb && heli_grocket_cd <= 0.f) {
            heli_grocket_cd = 1.2f;
            fire_rocket(gun + nose * 1.0f, front);
          }
        } else if (ai_ready) {
          const glm::vec3 ep(enemies[tgt].pos.x, enemies[tgt].pos.y + 1.0f, enemies[tgt].pos.z);
          const glm::vec3 dir = glm::normalize(ep - gun);
          if (heli_gun_cd <= 0.f) {
            heli_gun_cd = 0.11f;  // ~550 rpm cannon
            gunner_engaging = true;
            const glm::vec3 sd = glm::normalize(
                dir + glm::vec3((std::rand() % 100 - 50) / 900.f, (std::rand() % 100 - 50) / 900.f,
                                (std::rand() % 100 - 50) / 900.f));
            const auto eh = shoot_enemies(gun, sd, range + 2.f);
            if (eh.idx >= 0) {
              damage_enemy(eh.idx, eh.zone);
              tracers.push_back({gun, eh.point, 0.05f});
              impacts.push_back({eh.point, 0.35f});
            } else {
              tracers.push_back({gun, gun + sd * range, 0.05f});
            }
          }
          if (heli_grocket_cd <= 0.f && range < 160.f) {
            heli_grocket_cd = 4.0f;  // periodic rocket salvo at armour/clusters
            fire_rocket(gun + nose * 1.0f, dir);
          }
        }
      } else {
        gunner_target = -1;
        gunner_acquire = 0.f;
      }
    }

    // Advance / expire flares (countermeasure pyrotechnics): they fall under
    // gravity and leave a bright trail rendered via the tracer effect.
    for (auto& fl : flares) {
      fl.life -= dt;
      fl.v.y -= 6.f * dt;
      const glm::vec3 np = fl.p + fl.v * dt;
      tracers.push_back({fl.p, np, 0.05f});
      fl.p = np;
    }
    flares.erase(std::remove_if(flares.begin(), flares.end(),
                                [](const Flare& f) { return f.life <= 0.f; }),
                 flares.end());

    // Deterministic timestep for missile flight in headless capture so --shot can
    // reproduce a launch; live gameplay uses the real frame dt.
    const float mdt = shot_path.empty() ? dt : (1.f / 60.f);
    if (shot_missile && frame_no == std::max(2, shot_frames - 12)) launch_requested = true;

    // --- Missile launcher (R): fire a guided/ballistic missile from the nearest
    // vehicle. Locks onto an enemy under the crosshair if there is one, else
    // arcs toward the aimed ground point (or lofts ballistically at the sky). ---
    missile_reload = std::max(0.f, missile_reload - dt);
    if (launch_requested && missile_reload <= 0.f) {
      missile_reload = 1.4f;
      // Launch from the nearest placed vehicle's turret; fall back to the
      // player's shoulder so the launcher still works with no vehicle nearby.
      glm::vec3 launch = eye + front * 0.8f - glm::vec3(0.f, 0.2f, 0.f);
      float best_d2 = 70.f * 70.f;
      for (const auto& v : vehicles) {
        const glm::vec3 d = v.origin - eye;
        const float d2 = glm::dot(d, d);
        if (d2 < best_d2) {
          best_d2 = d2;
          launch = v.origin + glm::vec3(0.f, 2.6f, 0.f);  // ~turret/tube height
        }
      }
      ActiveMissile am;
      am.m.position = launch;
      // Aim: lock an enemy under the crosshair, else the aimed surface point,
      // else an unguided ballistic loft when pointed at open sky.
      const auto th = world.raycast({eye.x, eye.y, eye.z}, {front.x, front.y, front.z}, 2000.f);
      const float terr = th.hit ? th.distance : 2000.f;
      const auto eh = shoot_enemies(eye, front, terr);
      glm::vec3 aim_point;
      if (eh.idx >= 0) {
        am.homing_enemy = eh.idx;
        am.m.guided = true;
        am.m.has_target = true;
        aim_point = glm::vec3(enemies[eh.idx].pos.x, enemies[eh.idx].pos.y + 1.0f,
                              enemies[eh.idx].pos.z);
      } else if (th.hit) {
        am.m.guided = true;
        am.m.has_target = true;
        aim_point = glm::vec3(th.point.x, th.point.y, th.point.z);
      } else {
        am.m.guided = false;  // ballistic arc
        am.m.has_target = false;
        aim_point = eye + front * 800.f;
      }
      am.m.target = aim_point;
      glm::vec3 dir0 = aim_point - launch;
      if (glm::length(dir0) < 1e-3f) dir0 = front;
      dir0 = glm::normalize(dir0);
      am.m.velocity = dir0 * 55.f;  // tube-exit speed; the motor accelerates it
      if (shot_missile) {
        // Headless demo: fire from the camera straight down the (now open) view so
        // the round's flight path + exhaust trail are clearly visible in frame.
        am.m.position = eye;
        am.prev_pos = eye;
        am.m.boost_accel = 130.f;
        am.m.max_speed = 260.f;
      }
      am.prev_pos = launch;
      missiles.push_back(am);
      if (std::getenv("BF2_MISSILE_DEBUG")) {
        std::fprintf(stderr, "[missile] LAUNCH frame=%d pos=%.1f,%.1f,%.1f vel=%.1f,%.1f,%.1f\n",
                     frame_no, am.m.position.x, am.m.position.y, am.m.position.z, am.m.velocity.x,
                     am.m.velocity.y, am.m.velocity.z);
      }
      // Launch signature: bright flash + an exhaust cloud back down the tube.
      explosions.push_back({launch, 0.f, 0.25f});
      for (int i = 0; i < 6; ++i) {
        smoke.push_back({launch - dir0 * (0.3f * static_cast<float>(i)), 0.f, 1.2f});
      }
    }

    // --- Integrate live missiles: guidance + boost + gravity, spawn the exhaust
    // trail, and detonate on contact (segment raycast), proximity, or ground. ---
    for (auto& am : missiles) {
      if (!am.m.alive) continue;
      // Keep homing onto the tracked enemy's chest; drop the lock if it dies.
      if (am.homing_enemy >= 0 && am.homing_enemy < static_cast<int>(enemies.size())) {
        const Enemy& te = enemies[am.homing_enemy];
        if (te.alive) {
          am.m.target = glm::vec3(te.pos.x, te.pos.y + 1.0f, te.pos.z);
        } else {
          am.homing_enemy = -1;
        }
      }
      am.prev_pos = am.m.position;
      am.m.update(mdt);

      const glm::vec3 seg = am.m.position - am.prev_pos;
      const float seg_len = glm::length(seg);
      bool detonate = false;
      glm::vec3 boom = am.m.position;
      if (seg_len > 1e-4f) {
        const glm::vec3 sd = seg / seg_len;
        const auto hit = world.raycast({am.prev_pos.x, am.prev_pos.y, am.prev_pos.z},
                                       {seg.x, seg.y, seg.z}, seg_len);
        const float terr_d = hit.hit ? hit.distance : 1e30f;
        const auto eh = shoot_enemies(am.prev_pos, sd, seg_len);
        if (eh.idx >= 0 && eh.dist < terr_d) {
          detonate = true;
          boom = eh.point;
        } else if (hit.hit) {
          detonate = true;
          boom = glm::vec3(hit.point.x, hit.point.y, hit.point.z);
        }
      }
      if (!detonate && am.homing_enemy >= 0 &&
          am.homing_enemy < static_cast<int>(enemies.size())) {
        const Enemy& te = enemies[am.homing_enemy];
        const glm::vec3 chest(te.pos.x, te.pos.y + 1.f, te.pos.z);
        if (te.alive && glm::length(chest - am.m.position) < 2.5f) {
          detonate = true;
          boom = am.m.position;
        }
      }
      if (!detonate) {
        const float gh = world.terrain_height(am.m.position.x, am.m.position.z);
        if (am.m.position.y <= gh + 0.2f) {
          detonate = true;
          boom = glm::vec3(am.m.position.x, gh, am.m.position.z);
        }
      }
      if (!am.m.alive) {  // reached end of life -> airburst
        detonate = true;
        boom = am.m.position;
      }

      // Exhaust trail.
      am.smoke_timer -= mdt;
      if (am.smoke_timer <= 0.f) {
        am.smoke_timer = 0.012f;
        if (smoke.size() < 6000) {
          smoke.push_back({am.m.position - am.m.forward() * 0.6f, 0.f, 1.5f});
        }
      }

      if (detonate) {
        am.m.alive = false;
        explosions.push_back({boom, 0.f, 0.7f});
        explode_at(boom, 9.f, 150.f);
        for (int i = 0; i < 10; ++i) smoke.push_back({boom, 0.f, 1.8f});
        if (std::getenv("BF2_MISSILE_DEBUG")) {
          std::fprintf(stderr, "[missile] DETONATE frame=%d at %.1f,%.1f,%.1f age=%.2f\n", frame_no,
                       boom.x, boom.y, boom.z, am.m.age);
        }
      }
    }
    for (auto& s : smoke) s.age += mdt;
    for (auto& ex : explosions) ex.age += mdt;
    smoke.erase(std::remove_if(smoke.begin(), smoke.end(),
                               [](const Smoke& s) { return s.age >= s.life; }),
                smoke.end());
    explosions.erase(std::remove_if(explosions.begin(), explosions.end(),
                                    [](const Explosion& ex) { return ex.age >= ex.life; }),
                     explosions.end());
    missiles.erase(std::remove_if(missiles.begin(), missiles.end(),
                                  [](const ActiveMissile& a) { return !a.m.alive; }),
                   missiles.end());

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
        explosions.push_back({boom, 0.f, 0.5f});
        for (int s = 0; s < 5; ++s) smoke.push_back({boom, 0.f, 1.2f});
        // Grenades hurt the player too if they're close.
        if (!deploy_open) {
          const float d = glm::length(glm::vec3(player.position.x, player.position.y - 1.f,
                                                player.position.z) - boom);
          if (d < 8.f) player_health -= (1.f - d / 8.f) * 80.f;
        }
        gr.fuse = -1.f;  // mark for removal
      }
      (void)prev;
    }
    live_grenades.erase(std::remove_if(live_grenades.begin(), live_grenades.end(),
                                       [](const Grenade& g) { return g.fuse <= -0.5f; }),
                        live_grenades.end());

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
    constexpr float kSightRange = 65.f;   // can spot/advance from this far
    constexpr float kEngageRange = 40.f;  // only opens fire once this close
    constexpr int kMaxShooters = 2;       // simultaneous attackers cap
    // Gather living enemies with LOS, sorted by distance; only the nearest few fire.
    struct Contact { int idx; float dist; };
    std::vector<Contact> contacts;
    for (std::size_t i = 0; i < enemies.size(); ++i) {
      Enemy& en = enemies[i];
      if (!en.alive) {
        en.death_time += dt;
        if (en.death_time > 330.f) {  // respawn only after ~5.5 min: clear the map first
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
      // A slow ramp gives the player a real window to react or break contact —
      // BF2-bot "sluggish", not an instant-lock aimbot.
      en.alert = std::clamp(en.alert + (los ? dt / 1.6f : -dt / 2.5f), 0.f, 1.f);
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
      en.shot_timer = 0.14f;  // ~430 rpm within a burst
      if (--en.burst_left <= 0) en.burst_cooldown = 2.4f + frand() * 2.4f;  // longer pause between bursts
      const glm::vec3 chest = en.pos + glm::vec3(0.f, 1.35f, 0.f);
      const glm::vec3 dir = glm::normalize(eye - chest);
      const glm::vec3 muz = chest + dir * 0.3f;
      const float sp = 0.7f + dist * 0.09f;  // spread grows quickly with range
      const glm::vec3 aim =
          eye + glm::vec3((frand() - 0.5f), (frand() - 0.5f), (frand() - 0.5f)) * sp;
      tracers.push_back({muz, aim, 0.05f});
      // Accuracy falls off sharply with distance so anything past point-blank is a
      // real gamble; damage per round is modest so health regen keeps you alive
      // through a burst or two if you take cover.
      const float phit = std::clamp(0.42f - dist / 70.f, 0.02f, 0.3f);
      if (frand() < phit && !deploy_open) {
        player_health -= 4.f + frand() * 4.f;
        player_regen_delay = 3.f;
      }
    }
    if (player_health <= 0.f && !deploy_open) {
      // Killed: open the deploy screen so you re-pick kit/spawn (BF2 style).
      ++player_deaths;
      player_health = 100.f;
      deploy_open = true;
    }

    // ---- Multiplayer: publish our state, ingest remote shots ----------------
    if (net.active()) {
      bf2::NetPlayer me;
      me.id = net.local_id();
      me.x = player.position.x;
      me.y = player.position.y - player.eye_height;  // feet, matching body render
      me.z = player.position.z;
      me.yaw = std::atan2(flat_front.x, flat_front.z);  // radians (body facing)
      me.pitch = pitch;
      const float spd = std::sqrt(player.desired_velocity.x * player.desired_velocity.x +
                                  player.desired_velocity.z * player.desired_velocity.z);
      me.anim = spd > 8.f ? 2 : (spd > 0.3f ? 1 : 0);
      me.flags = net_fired ? 1 : 0;
      me.health = static_cast<std::int16_t>(std::clamp(player_health, -32000.f, 32000.f));
      me.faction_id = static_cast<std::uint16_t>(player_faction_id);
      me.active = true;
      bf2::NetShot sh;
      if (net_fired) {
        sh.shooter = net.local_id();
        sh.ox = net_fire_o.x; sh.oy = net_fire_o.y; sh.oz = net_fire_o.z;
        sh.dx = net_fire_d.x; sh.dy = net_fire_d.y; sh.dz = net_fire_d.z;
      }
      net.send_local(me, net_fired ? &sh : nullptr);
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
        if (!v.is_heli && v.wheels_on_ground) {
          // Runway chase: level view while taxiing (BF2 — mouse not flying yet).
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
    const glm::mat4 proj = glm::perspective(
        glm::radians(cam_fov), static_cast<float>(cur_w) / std::max(cur_h, 1), 0.2f, 9000.f);
    const glm::mat4 view = glm::lookAt(cam, cam + cam_front, cam_up);
    const glm::mat4 view_proj = proj * view;
    // Refit the shadow cascades to this frame's camera and sun (matrices ready
    // fit the cascades to this frame's camera + sun, then render depth for each.
    // Cover a longer shadow range now that far geometry is visible.
    csm.update(view, glm::radians(cam_fov), static_cast<float>(cur_w) / std::max(cur_h, 1), 0.2f,
               900.f, atmo.sun_dir);
    const glm::mat4 inv_view_proj = glm::inverse(view_proj);

    // ---- Shadow depth passes: buildings + vehicles cast into each cascade. ----
    const float shadow_cast_dist2 = 500.f * 500.f;  // distant buildings cast too now
    float cascade_vp[bf2::Renderer::kShadowCascades * 16];
    float cascade_splits[4];
    if (settings.shadows_enabled) {
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
          if (vit != vehicle_cache.end() && vit->second.vao != 0)
            renderer.draw_depth(vit->second, glm::value_ptr(v.model));
          for (const auto& part : v.parts) {
            const auto pit = vehicle_cache.find(part.mesh_key);
            if (pit == vehicle_cache.end() || pit->second.vao == 0) continue;
            renderer.draw_depth(pit->second, glm::value_ptr(v.model * part.local));
          }
        }
      }
    }
    renderer.set_shadows(cascade_vp, cascade_splits, settings.shadows_enabled);

    const float frame_t =
        static_cast<float>(now) / static_cast<float>(SDL_GetPerformanceFrequency());
    dalian::sync_drawable_size(window, cur_w, cur_h);
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

    // Vehicles (static props at their spawn points): body + attached child parts.
    for (const auto& v : vehicles) {
      const glm::vec3 d = v.origin - cam;
      if (glm::dot(d, d) > draw_dist2) continue;
      const auto vit = vehicle_cache.find(v.mesh_key);
      if (vit != vehicle_cache.end() && vit->second.vao != 0) {
        const glm::mat4 mvp = view_proj * v.model;
        // Cull back faces on the hull so you don't see the interior back-faces
        // through hatches/viewports (the "see inside the vehicle" artifact).
        renderer.draw_textured(vit->second, glm::value_ptr(mvp), glm::value_ptr(v.model), 0, nullptr,
                               true);
        ++drawn;
      }
      for (std::size_t wi = 0; wi < v.wheels.size(); ++wi) {
        const auto& wslot = v.wheels[wi];
        const auto wit = vehicle_cache.find(wslot.mesh_key);
        if (wit == vehicle_cache.end() || wit->second.vao == 0) continue;
        glm::mat4 pm = v.model * wslot.rest;
        if (wslot.steers) pm = glm::rotate(pm, v.visual_steer, glm::vec3(0, 1, 0));
        const float spin = wi < v.wheel_spin.size() ? v.wheel_spin[wi] : 0.f;
        pm = glm::rotate(pm, spin, glm::vec3(1, 0, 0));
        const glm::mat4 mvp = view_proj * pm;
        renderer.draw_textured(wit->second, glm::value_ptr(mvp), glm::value_ptr(pm));
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

    // Third-person animated soldier: pick a locomotion clip by movement speed and
    // drive the GPU skinning palette; body is placed at the player's feet facing
    // the look direction.
    if (third_person && have_soldier && in_vehicle < 0) {
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

    // Remote players (multiplayer): draw each other client's soldier at its
    // smoothed transform, animated by the replicated move state, tinted bluish
    // so teammates read distinctly from the red opfor.
    if (have_soldier && net.active()) {
      for (const auto& rp : net.players()) {
        if (!rp.active || rp.id == net.local_id()) continue;
        const glm::vec3 rpos(rp.rx, rp.ry, rp.rz);
        if (glm::dot(rpos - cam, rpos - cam) > draw_dist2) continue;
        const bf2::AnimationClip* clip =
            (rp.anim == 2 && have_clip_run)    ? &clip_run
            : (rp.anim == 1 && have_clip_walk) ? &clip_walk
            : have_clip_stand                  ? &clip_stand
                                               : nullptr;
        int frame = 0;
        if (clip && clip->frame_count > 0) {
          frame = static_cast<int>(anim_time * 30.f) % clip->frame_count;
        }
        const auto palette = bf2::compute_skin_palette(soldier_src, soldier_ske, clip, frame, 1, 0);
        if (palette.empty()) continue;
        glm::mat4 body = glm::translate(glm::mat4(1.0f), rpos);
        body = glm::rotate(body, rp.ryaw, glm::vec3(0, 1, 0));
        const glm::mat4 body_mvp = view_proj * body;
        const std::uint16_t rfid =
            rp.faction_id ? rp.faction_id : net.lobby_player_faction(rp.id);
        const int rside = dalian::faction_kit_side(static_cast<int>(rfid));
        const glm::vec3 tint = rside == 0 ? glm::vec3(0.62f, 0.78f, 1.25f)
                                          : glm::vec3(1.25f, 0.72f, 0.55f);
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
    if (undergrowth.valid() && grass_atlas_tex != 0) {
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

    // Missiles: draw the bundledmesh oriented along its flight path (nose = mesh
    // +Z). If no missile mesh loaded, fall back to a bright body streak.
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
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), am.m.position) * rot;
        const glm::mat4 mvp = view_proj * model;
        renderer.draw_textured(missile_mesh, glm::value_ptr(mvp), glm::value_ptr(model));
      }
    } else if (!missiles.empty()) {
      std::vector<float> mv;
      mv.reserve(missiles.size() * 6);
      for (const auto& am : missiles) {
        const glm::vec3 a = am.m.position;
        const glm::vec3 b = a - am.m.forward() * 2.5f;
        mv.insert(mv.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
      }
      renderer.draw_lines(glm::value_ptr(view_proj), mv.data(),
                          static_cast<int>(mv.size() / 3), 1.0f, 0.7f, 0.2f, 4.0f, true);
    }

    // Exhaust / launch smoke: grey line-stars that expand and fade with age.
    if (!smoke.empty()) {
      std::vector<float> sv;
      sv.reserve(smoke.size() * 18);
      for (const auto& s : smoke) {
        const float k = s.life > 0.f ? s.age / s.life : 1.f;
        const float sz = 0.25f + k * 1.6f;
        const glm::vec3 p = s.p;
        sv.insert(sv.end(), {p.x - sz, p.y, p.z, p.x + sz, p.y, p.z});
        sv.insert(sv.end(), {p.x, p.y - sz, p.z, p.x, p.y + sz, p.z});
        sv.insert(sv.end(), {p.x, p.y, p.z - sz, p.x, p.y, p.z + sz});
      }
      const float g = 0.72f;
      renderer.draw_lines(glm::value_ptr(view_proj), sv.data(),
                          static_cast<int>(sv.size() / 3), g, g, g + 0.03f, 3.0f, true);
    }

    // Explosions: expanding bright orange stars.
    if (!explosions.empty()) {
      std::vector<float> ev;
      ev.reserve(explosions.size() * 30);
      for (const auto& ex : explosions) {
        const float k = ex.life > 0.f ? ex.age / ex.life : 1.f;
        const float sz = 0.5f + k * 6.0f;
        const glm::vec3 p = ex.p;
        ev.insert(ev.end(), {p.x - sz, p.y, p.z, p.x + sz, p.y, p.z});
        ev.insert(ev.end(), {p.x, p.y - sz, p.z, p.x, p.y + sz, p.z});
        ev.insert(ev.end(), {p.x, p.y, p.z - sz, p.x, p.y, p.z + sz});
        const float dz = sz * 0.7f;
        ev.insert(ev.end(), {p.x - dz, p.y - dz, p.z, p.x + dz, p.y + dz, p.z});
        ev.insert(ev.end(), {p.x - dz, p.y + dz, p.z, p.x + dz, p.y - dz, p.z});
      }
      renderer.draw_lines(glm::value_ptr(view_proj), ev.data(),
                          static_cast<int>(ev.size() / 3), 1.0f, 0.6f, 0.15f, 4.0f, true);
    }

    // First-person weapon viewmodel: rendered in view space over the scene with a
    // narrower FOV so it doesn't distort. The depth buffer is cleared first so
    // the weapon never clips into the world but still self-occludes correctly.
    if (!third_person && !drone_mode && in_vehicle < 0) {
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

      // Floating name + faction tags over remote players.
      if (net.active() && !deploy_open) {
        for (const auto& rp : net.players()) {
          if (!rp.active || rp.id == net.local_id()) continue;
          const glm::vec4 clip =
              view_proj * glm::vec4(rp.rx, rp.ry + 2.15f, rp.rz, 1.f);
          if (clip.w <= 0.05f) continue;
          const float invw = 1.f / clip.w;
          const float sx = (clip.x * invw * 0.5f + 0.5f) * static_cast<float>(cur_w);
          const float sy = (1.f - (clip.y * invw * 0.5f + 0.5f)) * static_cast<float>(cur_h);
          float dx = 0.f, dy = 0.f;
          renderer.ui_unproject(static_cast<int>(sx), static_cast<int>(sy), dx, dy);
          if (dx < 20.f || dx > W - 20.f || dy < 20.f || dy > H - 20.f) continue;
          const std::string pname = net.lobby_player_name(rp.id);
          const std::uint16_t fid =
              rp.faction_id ? rp.faction_id : net.lobby_player_faction(rp.id);
          const auto& fd = dalian::faction_at(static_cast<int>(fid));
          char tag[160];
          if (!pname.empty())
            std::snprintf(tag, sizeof(tag), "%s  |  %s", pname.c_str(), fd.country);
          else
            std::snprintf(tag, sizeof(tag), "%s", fd.country);
          const float tw = renderer.ui_text_width(tag, 1.15f);
          renderer.ui_rect(dx - tw * 0.5f - 6.f, dy - 10.f, tw + 12.f, 20.f, 0.04f, 0.05f, 0.07f,
                           0.72f);
          renderer.ui_text(dx - tw * 0.5f, dy - 8.f, 1.15f, tag, 0.88f, 0.92f, 0.96f, 1.f);
        }
      }

      if (deploy_open) {
        int lmx = 0, lmy = 0;
        SDL_GetMouseState(&lmx, &lmy);
        run_deploy_ui(lmx, lmy, deploy_click);
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
        renderer.ui_rect(20, 20, renderer.ui_text_width(buf, 1.7f) + 20, 30, 0.05f, 0.06f, 0.08f,
                         0.55f);
        renderer.ui_text(30, 27, 1.7f, buf, 0.65f, 0.82f, 1.0f, 1.f);
        // Multiplayer status badge under the faction line.
        if (net.active()) {
          char nb[64];
          if (net.is_server())
            std::snprintf(nb, sizeof(nb), "MP HOST  |  %d player(s)", net.peer_count());
          else if (net.connected())
            std::snprintf(nb, sizeof(nb), "MP CLIENT  |  %d peer(s)", net.peer_count());
          else
            std::snprintf(nb, sizeof(nb), "MP CONNECTING...");
          renderer.ui_text(30, 56, 1.3f, nb, 0.55f, 0.95f, 0.75f, 1.f);
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
                      has_medkit ? "    MEDKIT" : (has_at ? "    AT ROCKET" : ""));
        renderer.ui_text(W - renderer.ui_text_width(buf, 1.4f) - 30, H - 66, 1.4f, buf, 0.8f, 0.82f,
                         0.85f, 1.f);

        // "[E] Enter" prompt when standing next to a drivable vehicle.
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
        }
      } else if (in_vehicle >= 0 && vehicles[in_vehicle].is_air) {
        // Helicopter cockpit HUD: altitude, an artificial horizon read-out of
        // the current attitude, rocket/flare readiness and gunner status.
        const Vehicle& v = vehicles[in_vehicle];
        char buf[96];
        if (v.is_heli) {
          std::snprintf(buf, sizeof(buf), "ALT %.0f m    SPD %.0f km/h    RPM %.0f%%", v.pos.y,
                        glm::length(glm::vec3(v.vel.x, 0.f, v.vel.z)) * 3.6f, v.rotor_rpm * 100.f);
        } else {
          std::snprintf(buf, sizeof(buf), "ALT %.0f m    SPD %.0f km/h    THR %.0f%%", v.pos.y,
                        glm::length(v.vel) * 3.6f, v.throttle * 100.f);
        }
        renderer.ui_text(30, H - 46, 1.8f, buf, 0.7f, 0.9f, 0.75f, 1.f);
        std::snprintf(buf, sizeof(buf), "PITCH %+.0f   BANK %+.0f", -v.pitch, v.roll);
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
                                    : "mouse pitch/roll, A/D yaw, W/S throttle, B recall");
      } else if (in_vehicle >= 0) {
        const Vehicle& v = vehicles[in_vehicle];
        std::snprintf(title, sizeof(title),
                      "Project Dalian  |  DRIVING %s  |  %.0f %s  |  HP %.0f  |  %.0f fps  |  %s",
                      v.is_air ? "AIRCRAFT" : "GROUND",
                      v.is_air ? v.pos.y : v.speed * 3.6f,
                      v.is_air ? "m alt" : "km/h",
                      player_health, dt > 0 ? 1.f / dt : 0.f,
                      (v.is_air && v.is_heli)
                          ? "W/S collective, A/D yaw, mouse roll+pitch, LMB rockets, "
                            "X flares, F1-F2 seats, E exit"
                      : v.is_air
                          ? (v.wheels_on_ground
                                 ? "W throttle, A/D rudder, pull BACK mouse to rotate, E exit"
                                 : "W/S throttle, mouse pitch/roll, A/D rudder, Shift boost, E exit")
                          : "W/S drive, A/D steer, Shift boost, F1-F3 seats, E exit");
      } else {
        std::snprintf(title, sizeof(title),
                      "Project Dalian  |  %s  |  %s  |  HP %.0f  |  K %d  D %d  |  %.0f fps  |  "
                      "LMB fire, R reload, G nade, C c4, X boom, T missile, E enter, ENTER deploy, "
                      "V 1st/3rd, ESC pause",
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
