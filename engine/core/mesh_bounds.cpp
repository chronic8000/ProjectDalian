#include "mesh_bounds.hpp"

#include "engine/formats/mesh/bf2_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bf2 {

MeshLocalBounds mesh_local_bounds(ResourceManager& resources, const std::string& mesh_vpath,
                                  std::unordered_map<std::string, MeshLocalBounds>* cache) {
  if (cache) {
    if (const auto it = cache->find(mesh_vpath); it != cache->end()) return it->second;
  }
  MeshLocalBounds out;
  try {
    const auto m = resources.load_mesh(mesh_vpath);
    const auto data = MeshLoader::extract_textured(m, 0, 0);
    if (!data.vertices.empty()) {
      float mn_x = data.vertices[0].position.x, mx_x = mn_x;
      float mn_y = data.vertices[0].position.y, mx_y = mn_y;
      float mn_z = data.vertices[0].position.z, mx_z = mn_z;
      std::vector<float> ys;
      ys.reserve(data.vertices.size());
      for (const auto& vtx : data.vertices) {
        ys.push_back(vtx.position.y);
        mn_x = std::min(mn_x, vtx.position.x);
        mx_x = std::max(mx_x, vtx.position.x);
        mn_y = std::min(mn_y, vtx.position.y);
        mx_y = std::max(mx_y, vtx.position.y);
        mn_z = std::min(mn_z, vtx.position.z);
        mx_z = std::max(mx_z, vtx.position.z);
      }
      const std::size_t k =
          std::min(ys.size() - 1, static_cast<std::size_t>(ys.size() * 0.015f));
      std::nth_element(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(k), ys.end());
      out.min_y = ys[k];
      // Radius from origin to farthest AABB corner — keeps tall cranes visible
      // when the pivot is near the cull boundary.
      const float corners[8][3] = {
          {mn_x, mn_y, mn_z}, {mn_x, mn_y, mx_z}, {mn_x, mx_y, mn_z}, {mn_x, mx_y, mx_z},
          {mx_x, mn_y, mn_z}, {mx_x, mn_y, mx_z}, {mx_x, mx_y, mn_z}, {mx_x, mx_y, mx_z},
      };
      float r2 = 0.f;
      for (const auto& c : corners) {
        const float d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        r2 = std::max(r2, d2);
      }
      out.cull_radius = std::sqrt(r2);
    }
  } catch (...) {
  }
  if (cache) cache->emplace(mesh_vpath, out);
  return out;
}

}  // namespace bf2
