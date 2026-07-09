#include "mesh_bounds.hpp"

#include "engine/formats/mesh/bf2_mesh.hpp"

#include <algorithm>
#include <vector>

namespace bf2 {

float mesh_local_min_y(ResourceManager& resources, const std::string& mesh_vpath,
                       std::unordered_map<std::string, float>* cache) {
  if (cache) {
    if (const auto it = cache->find(mesh_vpath); it != cache->end()) return it->second;
  }
  float min_y = 0.f;
  try {
    const auto m = resources.load_mesh(mesh_vpath);
    const auto data = MeshLoader::extract_textured(m, 0, 0);
    if (!data.vertices.empty()) {
      std::vector<float> ys;
      ys.reserve(data.vertices.size());
      for (const auto& vtx : data.vertices) ys.push_back(vtx.position.y);
      const std::size_t k =
          std::min(ys.size() - 1, static_cast<std::size_t>(ys.size() * 0.015f));
      std::nth_element(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(k), ys.end());
      min_y = ys[k];
    }
  } catch (...) {
  }
  if (cache) cache->emplace(mesh_vpath, min_y);
  return min_y;
}

}  // namespace bf2
