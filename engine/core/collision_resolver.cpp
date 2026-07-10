#include "collision_resolver.hpp"

#include "engine/formats/collision/bf2_collision.hpp"
#include "engine/formats/mesh/bf2_road_mesh.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool replace_suffix(std::string& path, const char* from, const char* to) {
  const std::string f = from;
  if (path.size() < f.size()) return false;
  if (path.compare(path.size() - f.size(), f.size(), f) != 0) return false;
  path.replace(path.size() - f.size(), f.size(), to);
  return true;
}

void append_col_faces(const CollisionCol& col, std::vector<Float3>& out) {
  const std::size_t vcount = col.vertices.size();
  out.reserve(out.size() + col.faces.size() * 3);
  for (const auto& f : col.faces) {
    if (f.v1 < vcount && f.v2 < vcount && f.v3 < vcount) {
      out.push_back(col.vertices[f.v1]);
      out.push_back(col.vertices[f.v2]);
      out.push_back(col.vertices[f.v3]);
    }
  }
}

void append_render_mesh(const Mesh& mesh, std::vector<Float3>& out, std::size_t max_tris) {
  if (mesh.geometries.empty() || mesh.geometries[0].lods.empty()) return;
  const ExtractedMesh extracted = MeshLoader::extract_geometry(mesh, 0, 0);
  if (extracted.indices.size() < 3) return;
  const std::size_t tri_count = extracted.indices.size() / 3;
  const std::size_t stride = tri_count > max_tris ? (tri_count / max_tris) + 1 : 1;
  for (std::size_t t = 0; t + 2 < extracted.indices.size(); t += 3 * stride) {
    const std::uint32_t i0 = extracted.indices[t];
    const std::uint32_t i1 = extracted.indices[t + 1];
    const std::uint32_t i2 = extracted.indices[t + 2];
    if (i0 >= extracted.vertices.size() || i1 >= extracted.vertices.size() ||
        i2 >= extracted.vertices.size()) {
      continue;
    }
    out.push_back(extracted.vertices[i0].position);
    out.push_back(extracted.vertices[i1].position);
    out.push_back(extracted.vertices[i2].position);
  }
}

}  // namespace

std::string resolve_collision_vpath(const std::string& mesh_vpath) {
  std::string path = lower(mesh_vpath);
  if (replace_suffix(path, ".staticmesh", ".collisionmesh") ||
      replace_suffix(path, ".bundledmesh", ".collisionmesh") ||
      replace_suffix(path, ".skinnedmesh", ".collisionmesh") ||
      replace_suffix(path, ".mesh", ".collisionmesh")) {
    return path;
  }
  return {};
}

std::vector<Float3> load_collision_soup(ResourceManager& resources, const std::string& mesh_vpath,
                                        std::size_t max_render_tris) {
  std::vector<Float3> out;
  const std::string col_vpath = resolve_collision_vpath(mesh_vpath);
  if (!col_vpath.empty()) {
    if (const auto bytes = resources.read_bytes(col_vpath)) {
      try {
        const auto cm = CollisionLoader::load_from_memory(*bytes);
        // BF2 col types: 0=projectile, 1=vehicle, 2=soldier, 3=AI.
        // Vehicle hulls are often sealed boxes that close doorways/windows.
        // Prefer soldier (walk openings), then projectile (dense + usually open),
        // and only then vehicle. Never merge vehicle with soldier — that seals
        // arches like ch_wall_high_6m_door.
        auto append_type = [&](std::uint32_t type) {
          for (const auto& col : cm.cols) {
            if (col.col_type == type) append_col_faces(col, out);
          }
        };
        append_type(2);
        if (out.empty()) append_type(0);
        if (out.empty()) append_type(1);
        if (out.empty()) {
          for (const auto& col : cm.cols) {
            if (!col.faces.empty()) append_col_faces(col, out);
          }
        }
        if (!out.empty()) return out;
      } catch (...) {
      }
    }
  }

  // RoadCompiled meshes are not StaticMeshes — dedicated loader.
  if (const auto bytes = resources.read_bytes(mesh_vpath)) {
    if (is_road_compiled_bytes(*bytes)) {
      try {
        const auto road = load_road_compiled(*bytes);
        return road_compiled_collision_soup(road);
      } catch (...) {
      }
    }
  }

  try {
    const auto mesh = resources.load_mesh(mesh_vpath);
    // Roads/bridges need full deck density — never stride-downsample them.
    const bool dense = mesh_vpath.find("road") != std::string::npos ||
                       mesh_vpath.find("bridge") != std::string::npos;
    append_render_mesh(mesh, out, dense ? std::numeric_limits<std::size_t>::max() : max_render_tris);
  } catch (...) {
  }
  return out;
}

std::size_t count_collision_soup_tris(const std::vector<Float3>& soup) {
  return soup.size() / 3;
}

bool collision_resolver_self_test() {
  if (resolve_collision_vpath("foo/bar/baz.staticmesh") != "foo/bar/baz.collisionmesh") return false;
  if (resolve_collision_vpath("veh/hull.bundledmesh") != "veh/hull.collisionmesh") return false;
  if (resolve_collision_vpath("roads/main_compiled.mesh") != "roads/main_compiled.collisionmesh")
    return false;
  if (!resolve_collision_vpath("unknown.dds").empty()) return false;
  return true;
}

}  // namespace bf2
