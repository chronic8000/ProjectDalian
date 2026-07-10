#pragma once

#include "engine/core/resource_manager.hpp"

#include <string>
#include <unordered_map>

namespace bf2 {

struct MeshLocalBounds {
  float min_y = 0.f;
  // Conservative sphere radius from local origin (covers AABB after node xforms).
  float cull_radius = 0.f;
};

// Lowest vertex Y + cull radius in mesh local space (LOD0 geom0).
MeshLocalBounds mesh_local_bounds(ResourceManager& resources, const std::string& mesh_vpath,
                                  std::unordered_map<std::string, MeshLocalBounds>* cache = nullptr);

// Lowest vertex Y in mesh local space (LOD0 geom0). Used to rest props/vehicles on terrain.
inline float mesh_local_min_y(ResourceManager& resources, const std::string& mesh_vpath,
                              std::unordered_map<std::string, float>* cache = nullptr) {
  if (cache) {
    if (const auto it = cache->find(mesh_vpath); it != cache->end()) return it->second;
  }
  const float y = mesh_local_bounds(resources, mesh_vpath).min_y;
  if (cache) cache->emplace(mesh_vpath, y);
  return y;
}

}  // namespace bf2
