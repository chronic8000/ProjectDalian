#pragma once

#include "engine/core/resource_manager.hpp"

#include <string>
#include <unordered_map>

namespace bf2 {

// Lowest vertex Y in mesh local space (LOD0 geom0). Used to rest props/vehicles on terrain.
float mesh_local_min_y(ResourceManager& resources, const std::string& mesh_vpath,
                       std::unordered_map<std::string, float>* cache = nullptr);

}  // namespace bf2
