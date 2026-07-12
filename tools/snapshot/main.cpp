// Headless BF2 renderer: renders a real mesh OR a terrain heightmap from a real
// archive to a PNG using an offscreen OpenGL framebuffer (no visible window).
//
// Mesh mode:    bf2snapshot <archive.zip> <virtual/mesh/path> <out.png> [size] [geom] [lod]
// Terrain mode: bf2snapshot --terrain <archive.zip> <raw/path> <out.png> [size] [w] [h] [hscale] [xz] [step]
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <GL/glew.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <filesystem>
#include <unordered_map>

#include "engine/anim/pose.hpp"
#include "engine/anim/skinning.hpp"
#include "engine/core/atmosphere.hpp"
#include "engine/core/level_loader.hpp"
#include "engine/core/object_lightmaps.hpp"
#include "engine/core/undergrowth.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"
#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/archive/archive.hpp"
#include "engine/formats/dds/dds_loader.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/formats/terrain/terrain_colormap.hpp"
#include "engine/formats/terrain/terrain_loader.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/texture_cache.hpp"

namespace {

bf2::MeshKind mesh_kind(const std::string& path) {
  std::string l = path;
  std::transform(l.begin(), l.end(), l.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (l.ends_with("bundledmesh")) return bf2::MeshKind::Bundled;
  if (l.ends_with("skinnedmesh")) return bf2::MeshKind::Skinned;
  return bf2::MeshKind::Static;
}

// Build a renderable grid mesh from a terrain heightfield (downsampled by step).
// When centered, the grid is centred on the world origin to match BF2 world
// coordinates used by StaticObjects.con placements.
bf2::ExtractedMesh terrain_to_mesh(const bf2::Terrain& t, float xz_scale, int step,
                                   bool centered = false) {
  bf2::ExtractedMesh mesh;
  const int w = static_cast<int>(t.width);
  const int h = static_cast<int>(t.height);
  const int gw = (w + step - 1) / step;
  const int gh = (h + step - 1) / step;
  const float off_x = centered ? (w * 0.5f) * xz_scale : 0.f;
  const float off_z = centered ? (h * 0.5f) * xz_scale : 0.f;

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
      // Central-difference normal.
      const float hl = height_at(x - step, z);
      const float hr = height_at(x + step, z);
      const float hd = height_at(x, z - step);
      const float hu = height_at(x, z + step);
      glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * step * xz_scale, hd - hu));
      v.normal.x = n.x;
      v.normal.y = n.y;
      v.normal.z = n.z;
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

// Append one triangle with a flat face normal.
void add_triangle(bf2::ExtractedMesh& mesh, const glm::vec3& a, const glm::vec3& b,
                  const glm::vec3& c) {
  const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
  const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
  for (const glm::vec3& p : {a, b, c}) {
    bf2::ExtractedVertex v;
    v.position.x = p.x;
    v.position.y = p.y;
    v.position.z = p.z;
    v.normal.x = n.x;
    v.normal.y = n.y;
    v.normal.z = n.z;
    mesh.vertices.push_back(v);
  }
  mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}

// A "bone" spindle: a diamond cross-section stretched from parent P to child C.
void add_bone(bf2::ExtractedMesh& mesh, const glm::vec3& P, const glm::vec3& C) {
  const glm::vec3 dir = C - P;
  const float len = glm::length(dir);
  if (len < 1e-6f) {
    return;
  }
  const glm::vec3 d = dir / len;
  const glm::vec3 up = std::fabs(d.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  const glm::vec3 u = glm::normalize(glm::cross(d, up));
  const glm::vec3 v = glm::normalize(glm::cross(d, u));
  const float r = std::max(len * 0.10f, 0.004f);
  const glm::vec3 m = P + dir * 0.2f;
  const glm::vec3 ring[4] = {m + u * r, m + v * r, m - u * r, m - v * r};
  for (int i = 0; i < 4; ++i) {
    add_triangle(mesh, P, ring[i], ring[(i + 1) % 4]);
    add_triangle(mesh, C, ring[(i + 1) % 4], ring[i]);
  }
}

// Axis-aligned box from center with half-extents (used as level placement markers).
void add_box(bf2::ExtractedMesh& mesh, const glm::vec3& c, const glm::vec3& he) {
  const glm::vec3 v[8] = {
      c + glm::vec3(-he.x, -he.y, -he.z), c + glm::vec3(he.x, -he.y, -he.z),
      c + glm::vec3(he.x, -he.y, he.z),   c + glm::vec3(-he.x, -he.y, he.z),
      c + glm::vec3(-he.x, he.y, -he.z),  c + glm::vec3(he.x, he.y, -he.z),
      c + glm::vec3(he.x, he.y, he.z),    c + glm::vec3(-he.x, he.y, he.z)};
  static const int faces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1},
                                  {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
  for (const auto& f : faces) {
    add_triangle(mesh, v[f[0]], v[f[1]], v[f[2]]);
    add_triangle(mesh, v[f[0]], v[f[2]], v[f[3]]);
  }
}

// Append a source mesh transformed by a model matrix into a destination mesh.
void add_instance(bf2::ExtractedMesh& dst, const bf2::ExtractedMesh& src, const glm::mat4& model) {
  const std::uint32_t base = static_cast<std::uint32_t>(dst.vertices.size());
  const glm::mat3 normal_mat(model);
  for (const auto& v : src.vertices) {
    bf2::ExtractedVertex out = v;
    const glm::vec3 p = glm::vec3(model * glm::vec4(v.position.x, v.position.y, v.position.z, 1.0f));
    const glm::vec3 n =
        glm::normalize(normal_mat * glm::vec3(v.normal.x, v.normal.y, v.normal.z));
    out.position = {p.x, p.y, p.z};
    out.normal = {n.x, n.y, n.z};
    dst.vertices.push_back(out);
  }
  for (const auto idx : src.indices) {
    dst.indices.push_back(base + idx);
  }
}

// Append a textured source mesh transformed into world space, preserving each
// material submesh's texture map names for later texture binding.
void add_textured_instance(bf2::TexturedMeshData& dst, const bf2::TexturedMeshData& src,
                           const glm::mat4& model) {
  const std::uint32_t vbase = static_cast<std::uint32_t>(dst.vertices.size());
  const std::uint32_t ibase = static_cast<std::uint32_t>(dst.indices.size());
  const glm::mat3 normal_mat(model);
  for (const auto& v : src.vertices) {
    bf2::ExtractedVertex out = v;
    const glm::vec3 p = glm::vec3(model * glm::vec4(v.position.x, v.position.y, v.position.z, 1.0f));
    const glm::vec3 n = glm::normalize(normal_mat * glm::vec3(v.normal.x, v.normal.y, v.normal.z));
    out.position = {p.x, p.y, p.z};
    out.normal = {n.x, n.y, n.z};
    dst.vertices.push_back(out);
  }
  for (const auto idx : src.indices) {
    dst.indices.push_back(vbase + idx);
  }
  for (const auto& sub : src.submeshes) {
    bf2::TexturedSubmesh s = sub;
    s.index_offset = ibase + sub.index_offset;
    dst.submeshes.push_back(std::move(s));
  }
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

// BF2 skeletons include non-spatial marker nodes (e.g. "meshN") whose position
// field is a sentinel, not a real coordinate. Reject anything non-finite or huge.
bool is_real_position(const glm::vec3& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
         std::fabs(p.x) < 1e6f && std::fabs(p.y) < 1e6f && std::fabs(p.z) < 1e6f;
}

bf2::ExtractedMesh build_skeleton_mesh(const bf2::Skeleton& ske, const bf2::PosedSkeleton& posed) {
  bf2::ExtractedMesh mesh;
  for (std::size_t i = 0; i < ske.nodes.size(); ++i) {
    const int parent = ske.nodes[i].parent;
    if (parent < 0 || parent >= static_cast<int>(posed.world_positions.size())) {
      continue;
    }
    const glm::vec3& c = posed.world_positions[i];
    const glm::vec3& p = posed.world_positions[static_cast<std::size_t>(parent)];
    if (is_real_position(c) && is_real_position(p)) {
      add_bone(mesh, p, c);
    }
  }
  return mesh;
}

int render_to_png(const bf2::ExtractedMesh& extracted, const std::string& out_png, int size,
                  glm::vec3 bg = glm::vec3(0.10f, 0.12f, 0.16f)) {
  if (extracted.vertices.empty() || extracted.indices.empty()) {
    std::cerr << "No drawable geometry\n";
    return 1;
  }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

  SDL_Window* window = SDL_CreateWindow("bf2snapshot", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, size, size,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
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

  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::cerr << "Framebuffer incomplete\n";
    return 1;
  }

  bf2::Renderer renderer;
  renderer.initialize(window);
  auto gpu = renderer.upload_mesh(extracted);

  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(-std::numeric_limits<float>::max());
  for (const auto& v : extracted.vertices) {
    lo = glm::min(lo, glm::vec3(v.position.x, v.position.y, v.position.z));
    hi = glm::max(hi, glm::vec3(v.position.x, v.position.y, v.position.z));
  }
  const glm::vec3 center = (lo + hi) * 0.5f;
  const float radius = std::max(0.001f, glm::length(hi - lo) * 0.5f);
  const float dist = radius * 2.4f;
  const glm::vec3 eye = center + glm::normalize(glm::vec3(0.7f, 0.55f, 0.9f)) * dist;
  const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, radius * 0.02f, dist * 8.0f);
  const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0, 1, 0));
  const glm::mat4 mvp = proj * view;

  renderer.set_viewport(size, size);
  renderer.begin_frame(bg.r, bg.g, bg.b);
  renderer.draw_mesh(gpu, glm::value_ptr(mvp));
  glFinish();

  std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4);
  glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  std::vector<unsigned char> flipped(pixels.size());
  const int stride = size * 4;
  for (int y = 0; y < size; ++y) {
    std::copy_n(&pixels[static_cast<std::size_t>(y) * stride], stride,
                &flipped[static_cast<std::size_t>(size - 1 - y) * stride]);
  }
  const bool ok = stbi_write_png(out_png.c_str(), size, size, 4, flipped.data(), stride);

  renderer.destroy_mesh(gpu);
  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();

  if (!ok) {
    std::cerr << "Failed to write PNG: " << out_png << '\n';
    return 1;
  }
  std::cout << "Wrote " << out_png << " (" << size << "x" << size << ")\n";
  return 0;
}

// Render textured geometry (world-space verts) to a PNG via an offscreen FBO.
int render_textured_to_png(bf2::ResourceManager& resources, const bf2::TexturedMeshData& data,
                           const std::string& out_png, int size,
                           glm::vec3 bg = glm::vec3(0.53f, 0.66f, 0.82f)) {
  if (data.vertices.empty() || data.indices.empty()) {
    std::cerr << "No drawable geometry\n";
    return 1;
  }
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window* window = SDL_CreateWindow("bf2snapshot", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, size, size,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
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

  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::cerr << "Framebuffer incomplete\n";
    return 1;
  }

  bf2::Renderer renderer;
  renderer.initialize(window);
  bf2::TextureCache textures(resources, renderer);
  auto gpu = renderer.upload_textured(data);
  for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
    gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
    gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
    gpu.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map);
    gpu.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map);
    gpu.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map);
    gpu.submeshes[i].specular_tex = textures.get(data.submeshes[i].specular_map);
  }
  std::cout << "Textures loaded " << textures.loaded_count() << ", missing "
            << textures.missing_count() << '\n';

  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(-std::numeric_limits<float>::max());
  for (const auto& v : data.vertices) {
    lo = glm::min(lo, glm::vec3(v.position.x, v.position.y, v.position.z));
    hi = glm::max(hi, glm::vec3(v.position.x, v.position.y, v.position.z));
  }
  const glm::vec3 center = (lo + hi) * 0.5f;
  const float radius = std::max(0.001f, glm::length(hi - lo) * 0.5f);
  const float dist = radius * 2.4f;
  glm::vec3 eye_dir(0.7f, 0.55f, 0.9f);
  if (const char* ed = std::getenv("BF2_SNAP_EYE")) {
    float a = 0.7f, b = 0.55f, c = 0.9f;
    std::sscanf(ed, "%f,%f,%f", &a, &b, &c);
    eye_dir = glm::vec3(a, b, c);
  }
  const glm::vec3 eye = center + glm::normalize(eye_dir) * dist;
  const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, radius * 0.02f, dist * 8.0f);
  const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0, 1, 0));
  const glm::mat4 mvp = proj * view;

  renderer.set_viewport(size, size);
  renderer.begin_frame(bg.r, bg.g, bg.b);
  renderer.draw_textured(gpu, glm::value_ptr(mvp));
  glFinish();

  std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4);
  glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  std::vector<unsigned char> flipped(pixels.size());
  const int stride = size * 4;
  for (int y = 0; y < size; ++y) {
    std::copy_n(&pixels[static_cast<std::size_t>(y) * stride], stride,
                &flipped[static_cast<std::size_t>(size - 1 - y) * stride]);
  }
  const bool ok = stbi_write_png(out_png.c_str(), size, size, 4, flipped.data(), stride);

  renderer.destroy_textured(gpu);
  textures.clear();
  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  if (!ok) {
    std::cerr << "Failed to write PNG: " << out_png << '\n';
    return 1;
  }
  std::cout << "Wrote " << out_png << " (" << size << "x" << size << ")\n";
  return 0;
}

// Preview a first-person viewmodel: renders the mesh in view space (camera at
// origin looking down -Z) with the supplied model transform, exactly as Project Dalian
// would draw the held weapon. Used to dial in placement offline.
int render_viewmodel_to_png(bf2::ResourceManager& resources, const bf2::TexturedMeshData& data,
                            const glm::mat4& model, const std::string& out_png, int size) {
  if (data.vertices.empty() || data.indices.empty()) {
    std::cerr << "No drawable geometry\n";
    return 1;
  }
  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(-std::numeric_limits<float>::max());
  for (const auto& v : data.vertices) {
    lo = glm::min(lo, glm::vec3(v.position.x, v.position.y, v.position.z));
    hi = glm::max(hi, glm::vec3(v.position.x, v.position.y, v.position.z));
  }
  std::cout << "AABB min=(" << lo.x << "," << lo.y << "," << lo.z << ") max=(" << hi.x << "," << hi.y
            << "," << hi.z << ") size=(" << (hi.x - lo.x) << "," << (hi.y - lo.y) << ","
            << (hi.z - lo.z) << ")\n";

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window* window = SDL_CreateWindow("bf2snapshot", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, size, size,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) {
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) {
    return 1;
  }

  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    return 1;
  }

  bf2::Renderer renderer;
  renderer.initialize(window);
  bf2::TextureCache textures(resources, renderer);
  auto gpu = renderer.upload_textured(data);
  for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
    gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
    gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
  }

  const glm::mat4 proj = glm::perspective(glm::radians(55.0f), 1.0f, 0.01f, 50.0f);
  const glm::mat4 mvp = proj * model;

  renderer.set_viewport(size, size);
  renderer.begin_frame(0.30f, 0.35f, 0.42f);
  renderer.draw_textured(gpu, glm::value_ptr(mvp), glm::value_ptr(model));
  glFinish();

  std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4);
  glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  std::vector<unsigned char> flipped(pixels.size());
  const int stride = size * 4;
  for (int y = 0; y < size; ++y) {
    std::copy_n(&pixels[static_cast<std::size_t>(y) * stride], stride,
                &flipped[static_cast<std::size_t>(size - 1 - y) * stride]);
  }
  const bool ok = stbi_write_png(out_png.c_str(), size, size, 4, flipped.data(), stride);

  renderer.destroy_textured(gpu);
  textures.clear();
  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  if (!ok) {
    return 1;
  }
  std::cout << "Wrote " << out_png << " (" << size << "x" << size << ")\n";
  return 0;
}

int render_terrain_colormap_to_png(bf2::ResourceManager& resources, const bf2::Terrain& terrain,
                                   float xz, int step, const bf2::TerrainVisualConfig& terrain_cfg,
                                   const std::string& out_png, int size) {
  const auto atlases = bf2::TerrainColormapLoader::build_atlases(resources, terrain_cfg);
  if (atlases.colormap.pixels.empty()) {
    std::cerr << "Failed to stitch terrain colormap\n";
    return 1;
  }
  const auto data = bf2::terrain_to_textured_mesh(terrain, xz, step);
  if (data.vertices.empty()) {
    std::cerr << "No terrain geometry\n";
    return 1;
  }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window* window = SDL_CreateWindow("bf2snapshot", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, size, size,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) {
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) {
    return 1;
  }

  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

  bf2::Renderer renderer;
  renderer.initialize(window);
  auto ground = bf2::TerrainColormapLoader::upload(atlases);
  if (ground.colormap == 0) {
    std::cerr << "Failed to upload terrain colormap\n";
    return 1;
  }
  auto gpu = renderer.upload_textured(data);

  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(-std::numeric_limits<float>::max());
  for (const auto& v : data.vertices) {
    lo = glm::min(lo, glm::vec3(v.position.x, v.position.y, v.position.z));
    hi = glm::max(hi, glm::vec3(v.position.x, v.position.y, v.position.z));
  }
  const glm::vec3 center = (lo + hi) * 0.5f;
  const float radius = std::max(0.001f, glm::length(hi - lo) * 0.5f);
  const float dist = radius * 2.2f;
  const glm::vec3 eye = center + glm::normalize(glm::vec3(0.8f, 0.6f, 0.5f)) * dist;
  const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, radius * 0.02f, dist * 8.0f);
  const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0, 1, 0));
  const glm::mat4 mvp = proj * view;

  renderer.set_viewport(size, size);
  renderer.begin_frame(0.53f, 0.66f, 0.82f);
  const float world_m = static_cast<float>(terrain.width) * xz;
  bf2::Renderer::TerrainDraw td;
  td.colormap = ground.colormap;
  td.lightmap = ground.lightmap;
  td.mask1 = ground.mask1;
  td.mask2 = ground.mask2;
  td.detail0 = ground.detail0;
  td.detail1 = ground.detail1;
  td.detail2 = ground.detail2;
  td.detail_tiling = world_m > 0.f ? world_m / 6.f : 64.f;
  renderer.draw_terrain_colormap(gpu, glm::value_ptr(mvp), td);
  glFinish();

  std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4);
  glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  std::vector<unsigned char> flipped(pixels.size());
  const int stride = size * 4;
  for (int y = 0; y < size; ++y) {
    std::copy_n(&pixels[static_cast<std::size_t>(y) * stride], stride,
                &flipped[static_cast<std::size_t>(size - 1 - y) * stride]);
  }
  const bool ok = stbi_write_png(out_png.c_str(), size, size, 4, flipped.data(), stride);

  renderer.destroy_textured(gpu);
  bf2::TerrainColormapLoader::destroy(ground);
  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return ok ? 0 : 1;
}

// Render a single static object with its baked BF2 object lightmap applied, to
// verify the Lightmaps/Objects atlas + UV routing headlessly.
//   --objlm <objects_client.zip> <level_client.zip> <mesh_vpath> x y z <out.png> [size]
int render_object_lightmap_to_png(const std::string& objects_zip, const std::string& level_client,
                                  const std::string& mesh_vpath, glm::vec3 pos,
                                  const std::string& out_png, int size, bool lit) {
  bf2::ResourceManager resources;
  if (!resources.archives().mount(objects_zip)) {
    std::cerr << "Failed to mount objects archive\n";
    return 1;
  }
  resources.archives().mount(level_client);

  bf2::TexturedMeshData data;
  try {
    const auto mesh = resources.load_mesh(mesh_vpath);
    data = bf2::MeshLoader::extract_textured(mesh, 0, 0);
  } catch (const std::exception& e) {
    std::cerr << "Load failed: " << e.what() << '\n';
    return 1;
  }
  if (data.vertices.empty()) {
    std::cerr << "No geometry\n";
    return 1;
  }

  bf2::ObjectLightmaps obj_lm;
  if (const auto b = resources.read_bytes("Lightmaps/Objects/LightmapAtlas.tai")) {
    obj_lm =
        bf2::parse_object_lightmaps(std::string(reinterpret_cast<const char*>(b->data()), b->size()));
  }
  const std::string base = bf2::detail::basename_no_ext(mesh_vpath);
  const bf2::ObjLmEntry* entry = lit ? nullptr : obj_lm.find(base, pos, 4.0f);
  std::cout << "Lightmap for " << base << " @ " << pos.x << "/" << pos.y << "/" << pos.z << ": "
            << (entry ? "found atlas " + std::to_string(entry->atlas) : "NONE") << '\n';

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window* window = SDL_CreateWindow("bf2snapshot", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, size, size,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) return 1;
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) return 1;

  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

  bf2::Renderer renderer;
  renderer.initialize(window);
  bf2::TextureCache textures(resources, renderer);
  auto gpu = renderer.upload_textured(data);
  for (std::size_t i = 0; i < gpu.submeshes.size() && i < data.submeshes.size(); ++i) {
    gpu.submeshes[i].base_tex = textures.get(data.submeshes[i].base_map);
    gpu.submeshes[i].detail_tex = textures.get(data.submeshes[i].detail_map);
    gpu.submeshes[i].normal_tex = textures.get(data.submeshes[i].normal_map);
    gpu.submeshes[i].dirt_tex = textures.get(data.submeshes[i].dirt_map);
    gpu.submeshes[i].crack_tex = textures.get(data.submeshes[i].crack_map);
    gpu.submeshes[i].specular_tex = textures.get(data.submeshes[i].specular_map);
  }

  std::uint32_t lm_tex = 0;
  glm::vec4 lm_xform(1, 1, 0, 0);
  if (entry) {
    char path[128];
    std::snprintf(path, sizeof(path), "Lightmaps/Objects/LightmapAtlas%d.dds", entry->atlas);
    try {
      lm_tex = renderer.upload_texture(resources.load_texture(path));
      lm_xform = entry->xform;
    } catch (const std::exception&) {
    }
  }

  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(-std::numeric_limits<float>::max());
  for (const auto& v : data.vertices) {
    lo = glm::min(lo, glm::vec3(v.position.x, v.position.y, v.position.z));
    hi = glm::max(hi, glm::vec3(v.position.x, v.position.y, v.position.z));
  }
  const glm::vec3 center = (lo + hi) * 0.5f;
  const float radius = std::max(0.001f, glm::length(hi - lo) * 0.5f);
  const float dist = radius * 2.4f;
  const glm::vec3 eye = center + glm::normalize(glm::vec3(0.7f, 0.55f, 0.9f)) * dist;
  const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, radius * 0.02f, dist * 8.0f);
  const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0, 1, 0));
  const glm::mat4 mvp = proj * view;

  renderer.set_viewport(size, size);
  renderer.begin_frame(0.5f, 0.6f, 0.72f);
  renderer.draw_textured(gpu, glm::value_ptr(mvp), nullptr, lm_tex, glm::value_ptr(lm_xform));
  glFinish();

  std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4);
  glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  std::vector<unsigned char> flipped(pixels.size());
  const int stride = size * 4;
  for (int y = 0; y < size; ++y) {
    std::copy_n(&pixels[static_cast<std::size_t>(y) * stride], stride,
                &flipped[static_cast<std::size_t>(size - 1 - y) * stride]);
  }
  const bool ok = stbi_write_png(out_png.c_str(), size, size, 4, flipped.data(), stride);
  renderer.destroy_textured(gpu);
  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return ok ? 0 : 1;
}

// Eye-level horizon shot with the gradient sky, distance fog and water plane, so
// the sky/water feature can be verified headlessly.
int render_skyview_to_png(bf2::ResourceManager& resources, const bf2::Terrain& terrain, float xz,
                          int step, const bf2::TerrainVisualConfig& terrain_cfg,
                          const bf2::Atmosphere& atmo, const std::string& out_png, int size,
                          bool grass_mode = false) {
  const auto atlases = bf2::TerrainColormapLoader::build_atlases(resources, terrain_cfg);
  if (atlases.colormap.pixels.empty()) {
    std::cerr << "Failed to stitch terrain colormap\n";
    return 1;
  }
  const auto data = bf2::terrain_to_textured_mesh(terrain, xz, step);
  if (data.vertices.empty()) {
    std::cerr << "No terrain geometry\n";
    return 1;
  }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window* window = SDL_CreateWindow("bf2snapshot", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, size, size,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) {
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  glewExperimental = GL_TRUE;
  if (glewInit() != GLEW_OK) {
    return 1;
  }

  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

  bf2::Renderer renderer;
  renderer.initialize(window);
  auto ground = bf2::TerrainColormapLoader::upload(atlases);
  auto gpu = renderer.upload_textured(data);

  // Optional grass (for --skyview --grass verification).
  bf2::Undergrowth undergrowth;
  std::uint32_t grass_atlas_tex = 0;
  if (grass_mode) {
    const auto cfg = resources.read_bytes("Undergrowth.cfg");
    const auto raw = resources.read_bytes("Undergrowth.raw");
    const auto tai = resources.read_bytes("UndergrowthAtlas.tai");
    if (cfg && raw && tai) {
      undergrowth = bf2::parse_undergrowth(
          std::string(reinterpret_cast<const char*>(cfg->data()), cfg->size()), *raw,
          std::string(reinterpret_cast<const char*>(tai->data()), tai->size()), xz);
      try {
        grass_atlas_tex = renderer.upload_texture(resources.load_texture("UndergrowthAtlas0.dds"));
      } catch (const std::exception&) {
      }
      std::cout << "Grass: " << undergrowth.width << "x" << undergrowth.height << ", "
                << undergrowth.grass.size() << " materials, atlas " << grass_atlas_tex << '\n';
    }
  }

  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(-std::numeric_limits<float>::max());
  for (const auto& v : data.vertices) {
    lo = glm::min(lo, glm::vec3(v.position.x, v.position.y, v.position.z));
    hi = glm::max(hi, glm::vec3(v.position.x, v.position.y, v.position.z));
  }
  const glm::vec3 center = (lo + hi) * 0.5f;
  const float radius = std::max(1.f, glm::length(hi - lo) * 0.5f);
  // Aim across the lowest part of the map (where the sea sits) toward land so the
  // water plane, terrain and sky all appear in one frame.
  glm::vec3 lowest = center;
  float min_y = std::numeric_limits<float>::max();
  for (const auto& v : data.vertices) {
    if (v.position.y < min_y) {
      min_y = v.position.y;
      lowest = glm::vec3(v.position.x, v.position.y, v.position.z);
    }
  }
  glm::vec3 dir = lowest - center;
  dir.y = 0.f;
  dir = (glm::length(dir) > 1.f) ? glm::normalize(dir) : glm::vec3(1, 0, 0);
  glm::vec3 eye = center + dir * radius * 1.05f;
  eye.y = atmo.water_level + std::max(45.f, radius * 0.18f);
  glm::vec3 look(center.x, center.y, center.z);
  float fog_start = radius * 0.6f;
  float fog_end = radius * 3.5f;

  // Grass verification: drop the camera to eye-level over the nearest grassy,
  // above-water terrain vertex and look along the ground.
  if (grass_mode && undergrowth.valid()) {
    glm::vec3 spot = center;
    float best = std::numeric_limits<float>::max();
    for (const auto& v : data.vertices) {
      if (v.position.y <= atmo.water_level + 1.0f) continue;
      if (!undergrowth.grass_at(v.position.x, v.position.z)) continue;
      const float d = (v.position.x - center.x) * (v.position.x - center.x) +
                      (v.position.z - center.z) * (v.position.z - center.z);
      if (d < best) {
        best = d;
        spot = glm::vec3(v.position.x, v.position.y, v.position.z);
      }
    }
    eye = spot + glm::vec3(0.f, 1.7f, 0.f);
    look = eye + glm::vec3(1.f, -0.15f, 1.f);
    fog_start = 60.f;
    fog_end = 260.f;
  }

  const glm::vec3 front = glm::normalize(look - eye);
  const glm::mat4 proj = glm::perspective(glm::radians(62.0f), 1.0f, 1.0f, 20000.f);
  const glm::mat4 view = glm::lookAt(eye, eye + front, glm::vec3(0, 1, 0));
  const glm::mat4 view_proj = proj * view;
  const glm::mat4 inv_view_proj = glm::inverse(view_proj);

  renderer.set_viewport(size, size);
  renderer.begin_frame(atmo.horizon_color.x, atmo.horizon_color.y, atmo.horizon_color.z);
  renderer.set_environment(glm::value_ptr(eye), glm::value_ptr(atmo.sun_dir),
                           glm::value_ptr(atmo.horizon_color), fog_start, fog_end);
  renderer.draw_sky(glm::value_ptr(inv_view_proj), glm::value_ptr(eye),
                    glm::value_ptr(atmo.sky_color), glm::value_ptr(atmo.horizon_color));

  const float world_m = static_cast<float>(terrain.width) * xz;
  bf2::Renderer::TerrainDraw td;
  td.colormap = ground.colormap;
  td.lightmap = ground.lightmap;
  td.mask1 = ground.mask1;
  td.mask2 = ground.mask2;
  td.detail0 = ground.detail0;
  td.detail1 = ground.detail1;
  td.detail2 = ground.detail2;
  td.detail_tiling = world_m > 0.f ? world_m / 6.f : 64.f;
  renderer.draw_terrain_colormap(gpu, glm::value_ptr(view_proj), td);

  if (grass_mode && undergrowth.valid() && grass_atlas_tex != 0) {
    // Bilinear terrain height matching the engine's centered mapping.
    auto height_at = [&](float x, float z) -> float {
      const int W = static_cast<int>(terrain.width), H = static_cast<int>(terrain.height);
      if (W == 0 || H == 0) return 0.f;
      float gx = std::clamp(x / xz + W * 0.5f, 0.f, W - 1.001f);
      float gz = std::clamp(z / xz + H * 0.5f, 0.f, H - 1.001f);
      const int x0 = static_cast<int>(gx), z0 = static_cast<int>(gz);
      const int x1 = std::min(x0 + 1, W - 1), z1 = std::min(z0 + 1, H - 1);
      const float fx = gx - x0, fz = gz - z0;
      auto at = [&](int cx, int cz) {
        return terrain.samples[static_cast<std::size_t>(cz) * W + cx].height;
      };
      const float top = at(x0, z0) + (at(x1, z0) - at(x0, z0)) * fx;
      const float bot = at(x0, z1) + (at(x1, z1) - at(x0, z1)) * fx;
      return top + (bot - top) * fz;
    };
    std::vector<float> grass_verts;
    bf2::build_grass_vertices(undergrowth, height_at, eye.x, eye.z, 46.f, atmo.water_level,
                              grass_verts);
    std::cout << "Grass blades verts: " << grass_verts.size() / 6 << '\n';
    renderer.draw_grass(glm::value_ptr(view_proj), glm::value_ptr(eye), grass_atlas_tex,
                        grass_verts.data(), static_cast<int>(grass_verts.size() / 6), 1.0f);
  }

  if (atmo.has_water) {
    renderer.draw_water(glm::value_ptr(view_proj), atmo.water_level, eye.x, eye.z, 6000.f, 1.0f,
                        glm::value_ptr(atmo.water_color));
  }
  glFinish();

  std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4);
  glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  std::vector<unsigned char> flipped(pixels.size());
  const int stride = size * 4;
  for (int y = 0; y < size; ++y) {
    std::copy_n(&pixels[static_cast<std::size_t>(y) * stride], stride,
                &flipped[static_cast<std::size_t>(size - 1 - y) * stride]);
  }
  const bool ok = stbi_write_png(out_png.c_str(), size, size, 4, flipped.data(), stride);

  renderer.destroy_textured(gpu);
  bf2::TerrainColormapLoader::destroy(ground);
  renderer.shutdown();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const bool terrain_mode = argc >= 2 && std::strcmp(argv[1], "--terrain") == 0;
  const bool skeleton_mode = argc >= 2 && std::strcmp(argv[1], "--skeleton") == 0;
  const bool level_mode = argc >= 2 && std::strcmp(argv[1], "--level") == 0;
  const bool walk_mode = argc >= 2 && std::strcmp(argv[1], "--walk") == 0;
  const bool skin_mode = argc >= 2 && std::strcmp(argv[1], "--skin") == 0;
  const bool textured_mode = argc >= 2 && std::strcmp(argv[1], "--textured") == 0;
  const bool viewmodel_mode = argc >= 2 && std::strcmp(argv[1], "--viewmodel") == 0;
  const bool terraintex_mode = argc >= 2 && std::strcmp(argv[1], "--terraintex") == 0;
  const bool skyview_mode = argc >= 2 && std::strcmp(argv[1], "--skyview") == 0;
  const bool objlm_mode = argc >= 2 && std::strcmp(argv[1], "--objlm") == 0;
  const bool dumptex_mode = argc >= 2 && std::strcmp(argv[1], "--dumptex") == 0;

  if (objlm_mode) {
    if (argc < 9) {
      std::cerr << "Usage: bf2snapshot --objlm <objects_client.zip> <level_client.zip> "
                   "<mesh_vpath> x y z <out.png> [size] [--nolm]\n";
      return 1;
    }
    const glm::vec3 pos(std::atof(argv[5]), std::atof(argv[6]), std::atof(argv[7]));
    const int size = argc >= 10 && argv[9][0] != '-' ? std::atoi(argv[9]) : 512;
    bool lit = false;
    for (int i = 8; i < argc; ++i)
      if (std::strcmp(argv[i], "--nolm") == 0) lit = true;
    return render_object_lightmap_to_png(argv[2], argv[3], argv[4], pos, argv[8], size, lit);
  }

  if (dumptex_mode) {
    // bf2snapshot --dumptex <archive.zip> <virtual/tex/path.dds> <out.png>
    if (argc < 5) {
      std::cerr << "Usage: bf2snapshot --dumptex <archive.zip> <tex.dds> <out.png>\n";
      return 1;
    }
    bf2::ResourceManager resources;
    if (!resources.archives().mount(argv[2])) {
      std::cerr << "Failed to mount archive\n";
      return 1;
    }
    try {
      const auto tex = bf2::DdsLoader::decode_to_rgba8(resources.load_texture(argv[3]));
      if (tex.width == 0 || tex.pixels.empty()) {
        std::cerr << "Decode produced no pixels\n";
        return 1;
      }
      std::cout << "Decoded " << tex.width << "x" << tex.height << " ("
                << tex.pixels.size() << " bytes rgba)\n";
      stbi_write_png(argv[4], static_cast<int>(tex.width), static_cast<int>(tex.height), 4,
                     tex.pixels.data(), static_cast<int>(tex.width) * 4);
    } catch (const std::exception& e) {
      std::cerr << "Failed: " << e.what() << '\n';
      return 1;
    }
    return 0;
  }

  const bool dumpatlas_mode = argc >= 2 && std::strcmp(argv[1], "--dumpatlas") == 0;
  if (dumpatlas_mode) {
    // bf2snapshot --dumpatlas <level_server.zip> <out.png>  (CPU only, no GL)
    if (argc < 4) {
      std::cerr << "Usage: bf2snapshot --dumpatlas <level_server.zip> <out.png>\n";
      return 1;
    }
    const std::string server_zip = argv[2];
    bf2::ResourceManager resources;
    resources.archives().mount(server_zip);
    const auto client_path = std::filesystem::path(server_zip).parent_path() / "client.zip";
    if (std::filesystem::exists(client_path)) resources.archives().mount(client_path.string());
    const auto mod_root = std::filesystem::path(server_zip).parent_path().parent_path().parent_path();
    if (std::filesystem::exists(mod_root / "Common_client.zip"))
      resources.archives().mount((mod_root / "Common_client.zip").string());
    bf2::TerrainVisualConfig cfg;
    if (const auto tc = resources.read_bytes("Terrain.con")) {
      cfg = bf2::TerrainColormapLoader::parse_terrain_con(
          std::string(reinterpret_cast<const char*>(tc->data()), tc->size()));
    }
    const auto atlases = bf2::TerrainColormapLoader::build_atlases(resources, cfg);
    if (atlases.colormap.pixels.empty()) {
      std::cerr << "No colormap\n";
      return 1;
    }
    stbi_write_png(argv[3], static_cast<int>(atlases.colormap.width),
                   static_cast<int>(atlases.colormap.height), 4, atlases.colormap.pixels.data(),
                   static_cast<int>(atlases.colormap.width) * 4);
    std::cout << "Wrote colormap atlas " << atlases.colormap.width << "x" << atlases.colormap.height
              << '\n';
    return 0;
  }

  if (terraintex_mode) {
    if (argc < 4) {
      std::cerr << "Usage: bf2snapshot --terraintex <level_server.zip> <out.png> [size] [step]\n";
      return 1;
    }
    const std::string server_zip = argv[2];
    const std::string out_png = argv[3];
    const int size = argc >= 5 ? std::atoi(argv[4]) : 1024;
    const int step = argc >= 6 ? std::atoi(argv[5]) : 3;

    bf2::ResourceManager resources;
    if (!resources.archives().mount(server_zip)) {
      std::cerr << "Failed to mount server archive\n";
      return 1;
    }
    const auto client_path = std::filesystem::path(server_zip).parent_path() / "client.zip";
    if (!std::filesystem::exists(client_path) || !resources.archives().mount(client_path.string())) {
      std::cerr << "Failed to mount client archive\n";
      return 1;
    }
    // Mod root = Levels/<map>/server.zip -> up 2 dirs. Mount Common_client.zip
    // for the shared terrain detail textures.
    const auto mod_root = std::filesystem::path(server_zip).parent_path().parent_path().parent_path();
    const auto common_path = mod_root / "Common_client.zip";
    if (std::filesystem::exists(common_path)) {
      resources.archives().mount(common_path.string());
    }

    bf2::LevelLoader level_loader(resources);
    const auto level = level_loader.load_mounted_level(server_zip);
    if (!level.has_terrain) {
      std::cerr << "No terrain\n";
      return 1;
    }

    float xz = 2.0f;
    if (const auto hd = resources.read_bytes("Heightdata.con")) {
      const std::string script(reinterpret_cast<const char*>(hd->data()), hd->size());
      const auto pos = script.find("heightmap.setScale");
      if (pos != std::string::npos) {
        try {
          xz = std::stof(script.substr(pos + 19));
        } catch (...) {
        }
      }
    }

    bf2::TerrainVisualConfig cfg;
    if (const auto tc = resources.read_bytes("Terrain.con")) {
      const std::string script(reinterpret_cast<const char*>(tc->data()), tc->size());
      cfg = bf2::TerrainColormapLoader::parse_terrain_con(script);
    }
    auto ground = bf2::TerrainColormapLoader::build_atlases(resources, cfg);
    if (ground.colormap.pixels.empty()) {
      std::cerr << "Failed to load terrain colormap\n";
      return 1;
    }
    std::cout << "Colormap " << ground.tile_cols << "x" << ground.tile_rows << " tiles\n";
    return render_terrain_colormap_to_png(resources, level.terrain, xz, step, cfg, out_png, size);
  }

  if (skyview_mode) {
    if (argc < 4) {
      std::cerr << "Usage: bf2snapshot --skyview <level_server.zip> <out.png> [size] [step]\n";
      return 1;
    }
    const std::string server_zip = argv[2];
    const std::string out_png = argv[3];
    const int size = argc >= 5 && argv[4][0] != '-' ? std::atoi(argv[4]) : 1024;
    const int step = argc >= 6 && argv[5][0] != '-' ? std::atoi(argv[5]) : 3;

    bf2::ResourceManager resources;
    if (!resources.archives().mount(server_zip)) {
      std::cerr << "Failed to mount server archive\n";
      return 1;
    }
    const auto client_path = std::filesystem::path(server_zip).parent_path() / "client.zip";
    if (std::filesystem::exists(client_path)) {
      resources.archives().mount(client_path.string());
    }
    const auto mod_root = std::filesystem::path(server_zip).parent_path().parent_path().parent_path();
    const auto common_path = mod_root / "Common_client.zip";
    if (std::filesystem::exists(common_path)) {
      resources.archives().mount(common_path.string());
    }

    bf2::LevelLoader level_loader(resources);
    const auto level = level_loader.load_mounted_level(server_zip);
    if (!level.has_terrain) {
      std::cerr << "No terrain\n";
      return 1;
    }

    float xz = 2.0f;
    std::string heightdata_con;
    if (const auto hd = resources.read_bytes("Heightdata.con")) {
      heightdata_con.assign(reinterpret_cast<const char*>(hd->data()), hd->size());
      xz = bf2::parse_heightmap_xz_scale(heightdata_con, xz);
    }
    std::string water_con, sky_con;
    if (const auto b = resources.read_bytes("Water.con")) {
      water_con.assign(reinterpret_cast<const char*>(b->data()), b->size());
    }
    if (const auto b = resources.read_bytes("Sky.con")) {
      sky_con.assign(reinterpret_cast<const char*>(b->data()), b->size());
    }
    const bf2::Atmosphere atmo = bf2::parse_atmosphere(water_con, sky_con, heightdata_con);

    bf2::TerrainVisualConfig cfg;
    if (const auto tc = resources.read_bytes("Terrain.con")) {
      const std::string script(reinterpret_cast<const char*>(tc->data()), tc->size());
      cfg = bf2::TerrainColormapLoader::parse_terrain_con(script);
    }
    bool grass = false;
    for (int i = 4; i < argc; ++i)
      if (std::strcmp(argv[i], "--grass") == 0) grass = true;
    return render_skyview_to_png(resources, level.terrain, xz, step, cfg, atmo, out_png, size, grass);
  }

  if (textured_mode) {
    if (argc < 5) {
      std::cerr << "Usage: bf2snapshot --textured <archive.zip> <virtual/mesh/path> <out.png>"
                   " [size] [geom] [lod]\n";
      return 1;
    }
    const std::string archive_path = argv[2];
    const std::string mesh_vpath = argv[3];
    const std::string out_png = argv[4];
    const int size = argc >= 6 ? std::atoi(argv[5]) : 1024;
    const std::size_t geom = argc >= 7 ? static_cast<std::size_t>(std::atoi(argv[6])) : 0;
    const std::size_t lod = argc >= 8 ? static_cast<std::size_t>(std::atoi(argv[7])) : 0;

    bf2::ResourceManager resources;
    if (!resources.archives().mount(archive_path)) {
      std::cerr << "Failed to mount archive: " << archive_path << '\n';
      return 1;
    }
    const auto mesh = resources.load_mesh(mesh_vpath);
    auto data = bf2::MeshLoader::extract_textured(mesh, geom, lod);
    std::cout << "Loaded " << mesh_vpath << ": " << data.vertices.size() << " verts, "
              << data.indices.size() << " indices, " << data.submeshes.size() << " submeshes\n";
    return render_textured_to_png(resources, data, out_png, size);
  }

  if (viewmodel_mode) {
    // bf2snapshot --viewmodel <archive.zip> <mesh> <out.png> [size] [geom] [lod]
    //   [tx ty tz] [rx ry rz deg] [scale]
    if (argc < 5) {
      std::cerr << "Usage: bf2snapshot --viewmodel <archive.zip> <mesh> <out.png> [size] [geom]"
                   " [lod] [tx ty tz] [rx ry rz] [scale]\n";
      return 1;
    }
    const std::string archive_path = argv[2];
    const std::string mesh_vpath = argv[3];
    const std::string out_png = argv[4];
    const int size = argc >= 6 ? std::atoi(argv[5]) : 900;
    const std::size_t geom = argc >= 7 ? static_cast<std::size_t>(std::atoi(argv[6])) : 0;
    const std::size_t lod = argc >= 8 ? static_cast<std::size_t>(std::atoi(argv[7])) : 0;
    const float tx = argc >= 9 ? static_cast<float>(std::atof(argv[8])) : 0.f;
    const float ty = argc >= 10 ? static_cast<float>(std::atof(argv[9])) : 0.f;
    const float tz = argc >= 11 ? static_cast<float>(std::atof(argv[10])) : 0.f;
    const float rx = argc >= 12 ? static_cast<float>(std::atof(argv[11])) : 0.f;
    const float ry = argc >= 13 ? static_cast<float>(std::atof(argv[12])) : 0.f;
    const float rz = argc >= 14 ? static_cast<float>(std::atof(argv[13])) : 0.f;
    const float scale = argc >= 15 ? static_cast<float>(std::atof(argv[14])) : 1.f;

    bf2::ResourceManager resources;
    if (!resources.archives().mount(archive_path)) {
      std::cerr << "Failed to mount archive: " << archive_path << '\n';
      return 1;
    }
    const auto mesh = resources.load_mesh(mesh_vpath);
    auto data = bf2::MeshLoader::extract_textured(mesh, geom, lod);
    std::cout << "Loaded " << mesh_vpath << ": " << data.vertices.size() << " verts, "
              << data.submeshes.size() << " submeshes\n";
    glm::mat4 model(1.0f);
    model = glm::translate(model, glm::vec3(tx, ty, tz));
    model = glm::rotate(model, glm::radians(rx), glm::vec3(1, 0, 0));
    model = glm::rotate(model, glm::radians(ry), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(rz), glm::vec3(0, 0, 1));
    model = glm::scale(model, glm::vec3(scale));
    return render_viewmodel_to_png(resources, data, model, out_png, size);
  }

  if (skin_mode) {
    if (argc < 7) {
      std::cerr << "Usage: bf2snapshot --skin <archive.zip> <skinnedmesh> <ske> <baf|-> <out.png>"
                   " [size] [frame] [geom]\n";
      return 1;
    }
    const std::string archive_path = argv[2];
    const std::string mesh_vpath = argv[3];
    const std::string ske_vpath = argv[4];
    const std::string baf_vpath = argv[5];
    const std::string out_png = argv[6];
    const int size = argc >= 8 ? std::atoi(argv[7]) : 1024;
    const int frame = argc >= 9 ? std::atoi(argv[8]) : 0;
    const int geom = argc >= 10 ? std::atoi(argv[9]) : 0;

    bf2::ArchiveMount archive;
    if (!archive.mount(archive_path)) {
      std::cerr << "Failed to mount archive: " << archive_path << '\n';
      return 1;
    }
    const auto mesh_bytes = archive.read(mesh_vpath);
    const auto ske_bytes = archive.read(ske_vpath);
    if (!mesh_bytes || !ske_bytes) {
      std::cerr << "Mesh or skeleton not found\n";
      return 1;
    }
    const auto mesh = bf2::MeshLoader::load_from_memory(*mesh_bytes, bf2::MeshKind::Skinned);
    const auto skeleton = bf2::SkeletonLoader::load_from_memory(*ske_bytes);

    bf2::AnimationClip clip;
    const bf2::AnimationClip* clip_ptr = nullptr;
    if (baf_vpath != "-") {
      const auto baf_bytes = archive.read(baf_vpath);
      if (!baf_bytes) {
        std::cerr << "Animation not found: " << baf_vpath << '\n';
        return 1;
      }
      clip = bf2::AnimationLoader::load_from_memory(*baf_bytes);
      clip_ptr = &clip;
    }

    const auto skinned = bf2::skin_mesh(mesh, skeleton, clip_ptr, frame, geom, 0);
    std::cout << "Skinned " << skinned.vertices.size() << " verts, " << skinned.indices.size()
              << " indices against " << skeleton.nodes.size() << " bones";
    if (clip_ptr) {
      std::cout << " @ frame " << frame << "/" << clip.frame_count;
    }
    std::cout << '\n';
    return render_to_png(skinned, out_png, size);
  }

  if (walk_mode) {
    if (argc < 4) {
      std::cerr << "Usage: bf2snapshot --walk <level_server.zip> <out.png> [size] [steps] [speed]\n";
      return 1;
    }
    const std::string archive_path = argv[2];
    const std::string out_png = argv[3];
    const int size = argc >= 5 ? std::atoi(argv[4]) : 1024;
    const int steps = argc >= 6 ? std::atoi(argv[5]) : 1400;
    const float speed = argc >= 7 ? static_cast<float>(std::atof(argv[6])) : 22.0f;

    bf2::ResourceManager resources;
    if (!resources.archives().mount(archive_path)) {
      std::cerr << "Failed to mount level archive: " << archive_path << '\n';
      return 1;
    }
    bf2::LevelLoader loader(resources);
    const auto level = loader.load_mounted_level(archive_path);
    if (!level.has_terrain) {
      std::cerr << "No terrain in level\n";
      return 1;
    }

    float xz = 2.0f;
    if (const auto hd = resources.read_bytes("Heightdata.con")) {
      const std::string script(reinterpret_cast<const char*>(hd->data()), hd->size());
      const auto pos = script.find("heightmap.setScale");
      if (pos != std::string::npos) {
        try {
          xz = std::stof(script.substr(pos + 19));
        } catch (...) {
        }
      }
    }

    bf2::PhysicsWorld world;
    world.set_terrain(level.terrain, xz, /*centered=*/true);

    // Walk diagonally across the map so the path crosses hills and flats.
    bf2::CharacterController character;
    character.position = {-500.f, 0.f, -300.f};
    character.position.y = world.terrain_height(-500.f, -300.f) + character.eye_height + 50.f;
    const float diag = std::sqrt(0.5f);
    character.desired_velocity = {speed * diag, 0.f, speed * diag};

    auto terrain_mesh = terrain_to_mesh(level.terrain, xz, 3, /*centered=*/true);
    float min_h = 1e30f, max_h = -1e30f;
    int grounded = 0;
    const float dt = 1.0f / 30.0f;
    for (int i = 0; i < steps; ++i) {
      world.step_character(character, dt);
      if (character.on_ground) {
        ++grounded;
      }
      const float foot = character.position.y - character.eye_height;
      min_h = std::min(min_h, foot);
      max_h = std::max(max_h, foot);
      if (i % 6 == 0) {  // trace marker every few steps
        add_box(terrain_mesh,
                glm::vec3(character.position.x, foot + 3.f, character.position.z),
                glm::vec3(3.f, 3.f, 3.f));
      }
    }
    std::cout << "Walked " << steps << " steps; grounded " << grounded << "/" << steps
              << "; foot height range " << min_h << " .. " << max_h << '\n';
    return render_to_png(terrain_mesh, out_png, size);
  }

  if (level_mode) {
    if (argc < 4) {
      std::cerr << "Usage: bf2snapshot --level <level_server.zip> <out.png> [size] [step]\n";
      return 1;
    }
    const std::string archive_path = argv[2];
    const std::string out_png = argv[3];
    const int size = argc >= 5 ? std::atoi(argv[4]) : 1024;
    const int step = argc >= 6 ? std::atoi(argv[5]) : 4;
    // Objects archive holding the referenced static-object cons/meshes. Default:
    // walk up <mod>/Levels/<level>/server.zip -> <mod>/Objects_client.zip.
    std::string objects_zip = argc >= 7 ? argv[6] : std::string();
    if (objects_zip.empty()) {
      const auto p = std::filesystem::path(archive_path);
      if (p.has_parent_path() && p.parent_path().has_parent_path() &&
          p.parent_path().parent_path().has_parent_path()) {
        objects_zip =
            (p.parent_path().parent_path().parent_path() / "Objects_client.zip").string();
      }
    }

    bf2::ResourceManager resources;
    if (!resources.archives().mount(archive_path)) {
      std::cerr << "Failed to mount level archive: " << archive_path << '\n';
      return 1;
    }
    const bool have_objects =
        !objects_zip.empty() && std::filesystem::exists(objects_zip) &&
        resources.archives().mount(objects_zip);
    std::cout << "Objects archive: " << (have_objects ? objects_zip : "(none)") << '\n';

    bf2::LevelLoader loader(resources);
    const auto level = loader.load_mounted_level(archive_path);
    std::cout << "Level: terrain=" << (level.has_terrain ? "yes" : "no") << " placements="
              << level.placements.size() << '\n';
    if (!level.has_terrain) {
      std::cerr << "No terrain found in level archive\n";
      return 1;
    }

    // Recover horizontal spacing from Heightdata.con (defaults to Dalian's 2.0).
    float xz = 2.0f;
    if (const auto hd = resources.read_bytes("Heightdata.con")) {
      const std::string script(reinterpret_cast<const char*>(hd->data()), hd->size());
      const auto pos = script.find("heightmap.setScale");
      if (pos != std::string::npos) {
        try {
          xz = std::stof(script.substr(pos + 19));
        } catch (...) {
        }
      }
    }

    // Build template -> mesh map by following StaticObjects.con `run` lines.
    bf2::TemplateResolver resolver(resources);
    if (const auto so = resources.read_bytes("StaticObjects.con")) {
      const std::string script(reinterpret_cast<const char*>(so->data()), so->size());
      resolver.build_from_static_objects(script);
    }
    std::cout << "Resolved " << resolver.map().size() << " object templates to meshes\n";

    auto mesh = terrain_to_mesh(level.terrain, xz, step, /*centered=*/true);

    std::unordered_map<std::string, bf2::ExtractedMesh> geo_cache;
    int drawn = 0, markers = 0;
    for (const auto& inst : level.placements) {
      const std::string vpath = resolver.resolve_mesh(inst.template_name);
      if (vpath.empty()) {
        add_box(mesh, glm::vec3(inst.position[0], inst.position[1] + 6.f, inst.position[2]),
                glm::vec3(2.f, 6.f, 2.f));
        ++markers;
        continue;
      }
      auto it = geo_cache.find(vpath);
      if (it == geo_cache.end()) {
        bf2::ExtractedMesh extracted;
        try {
          const auto m = resources.load_mesh(vpath);
          const std::size_t lod =
              m.geometries.empty() ? 0 : m.geometries[0].lods.size() - 1;  // least detail
          extracted = bf2::MeshLoader::extract_geometry(m, 0, lod);
        } catch (const std::exception&) {
        }
        it = geo_cache.emplace(vpath, std::move(extracted)).first;
      }
      if (!it->second.vertices.empty()) {
        add_instance(mesh, it->second, placement_matrix(inst));
        ++drawn;
      } else {
        add_box(mesh, glm::vec3(inst.position[0], inst.position[1] + 6.f, inst.position[2]),
                glm::vec3(2.f, 6.f, 2.f));
        ++markers;
      }
    }
    std::cout << "Placed " << drawn << " real meshes, " << markers << " markers; assembled "
              << mesh.vertices.size() << " verts\n";
    return render_to_png(mesh, out_png, size);
  }

  if (skeleton_mode) {
    if (argc < 6) {
      std::cerr << "Usage: bf2snapshot --skeleton <archive.zip> <ske/path> <baf/path|-> <out.png>"
                   " [size] [frame]\n";
      return 1;
    }
    const std::string archive_path = argv[2];
    const std::string ske_vpath = argv[3];
    const std::string baf_vpath = argv[4];
    const std::string out_png = argv[5];
    const int size = argc >= 7 ? std::atoi(argv[6]) : 1024;
    const int frame = argc >= 8 ? std::atoi(argv[7]) : 0;

    bf2::ArchiveMount archive;
    if (!archive.mount(archive_path)) {
      std::cerr << "Failed to mount archive: " << archive_path << '\n';
      return 1;
    }
    const auto ske_bytes = archive.read(ske_vpath);
    if (!ske_bytes) {
      std::cerr << "Skeleton not found: " << ske_vpath << '\n';
      return 1;
    }
    const auto skeleton = bf2::SkeletonLoader::load_from_memory(*ske_bytes);

    bf2::AnimationClip clip;
    const bf2::AnimationClip* clip_ptr = nullptr;
    if (baf_vpath != "-") {
      const auto baf_bytes = archive.read(baf_vpath);
      if (!baf_bytes) {
        std::cerr << "Animation not found: " << baf_vpath << '\n';
        return 1;
      }
      clip = bf2::AnimationLoader::load_from_memory(*baf_bytes);
      clip_ptr = &clip;
    }

    const auto posed = bf2::pose_skeleton(skeleton, clip_ptr, frame);
    std::cout << "Posed " << skeleton.nodes.size() << " bones";
    if (clip_ptr) {
      std::cout << " at frame " << frame << "/" << clip.frame_count;
    }
    std::cout << '\n';
    if (std::getenv("BF2_DUMP_BONES")) {
      for (std::size_t i = 0; i < posed.world_positions.size(); ++i) {
        const auto& p = posed.world_positions[i];
        std::cout << "  [" << i << "] " << skeleton.nodes[i].name << " parent="
                  << skeleton.nodes[i].parent << " pos(" << p.x << "," << p.y << "," << p.z << ")\n";
      }
    }
    const auto mesh = build_skeleton_mesh(skeleton, posed);
    return render_to_png(mesh, out_png, size, glm::vec3(0.85f, 0.87f, 0.90f));
  }

  if (terrain_mode) {
    if (argc < 5) {
      std::cerr << "Usage: bf2snapshot --terrain <archive.zip> <raw/path> <out.png>"
                   " [size] [w] [h] [hscale] [xz] [step]\n";
      return 1;
    }
    const std::string archive_path = argv[2];
    const std::string raw_vpath = argv[3];
    const std::string out_png = argv[4];
    const int size = argc >= 6 ? std::atoi(argv[5]) : 1024;
    const std::uint32_t w = argc >= 7 ? std::atoi(argv[6]) : 1025;
    const std::uint32_t h = argc >= 8 ? std::atoi(argv[7]) : 1025;
    const float hscale = argc >= 9 ? std::atof(argv[8]) : 0.00640869f;
    const float xz = argc >= 10 ? std::atof(argv[9]) : 2.0f;
    const int step = argc >= 11 ? std::atoi(argv[10]) : 4;

    bf2::ArchiveMount archive;
    if (!archive.mount(archive_path)) {
      std::cerr << "Failed to mount archive: " << archive_path << '\n';
      return 1;
    }
    const auto bytes = archive.read(raw_vpath);
    if (!bytes) {
      std::cerr << "Heightmap not found: " << raw_vpath << '\n';
      return 1;
    }
    const auto terrain = bf2::TerrainLoader::load_raw_heightmap(*bytes, w, h, hscale);
    std::cout << "Loaded terrain " << w << "x" << h << " (step " << step << ")\n";
    const auto mesh = terrain_to_mesh(terrain, xz, step);
    return render_to_png(mesh, out_png, size);
  }

  if (argc < 4) {
    std::cerr << "Usage: bf2snapshot <archive.zip> <virtual/mesh/path> <out.png> [size] [geom] [lod]\n";
    return 1;
  }
  const std::string archive_path = argv[1];
  const std::string mesh_vpath = argv[2];
  const std::string out_png = argv[3];
  const int size = argc >= 5 ? std::atoi(argv[4]) : 1024;
  const std::size_t geom = argc >= 6 ? static_cast<std::size_t>(std::atoi(argv[5])) : 0;
  const std::size_t lod = argc >= 7 ? static_cast<std::size_t>(std::atoi(argv[6])) : 0;

  bf2::ArchiveMount archive;
  if (!archive.mount(archive_path)) {
    std::cerr << "Failed to mount archive: " << archive_path << '\n';
    return 1;
  }
  const auto bytes = archive.read(mesh_vpath);
  if (!bytes) {
    std::cerr << "Mesh not found: " << mesh_vpath << '\n';
    return 1;
  }
  const auto mesh = bf2::MeshLoader::load_from_memory(*bytes, mesh_kind(mesh_vpath));
  const auto extracted = bf2::MeshLoader::extract_geometry(mesh, geom, lod);
  std::cout << "Loaded " << mesh_vpath << ": " << extracted.vertices.size() << " verts, "
            << extracted.indices.size() << " indices\n";
  return render_to_png(extracted, out_png, size);
}
