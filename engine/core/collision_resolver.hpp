#pragma once

#include "engine/core/resource_manager.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"

#include <string>
#include <vector>

namespace bf2 {

// Map a render mesh path to a sibling .collisionmesh when present.
std::string resolve_collision_vpath(const std::string& mesh_vpath);

// Triangle soup in mesh-local space: collisionmesh cols (soldier + vehicle), else render mesh LOD0.
std::vector<Float3> load_collision_soup(ResourceManager& resources, const std::string& mesh_vpath,
                                        std::size_t max_render_tris = 60000);

std::size_t count_collision_soup_tris(const std::vector<Float3>& soup);

bool collision_resolver_self_test();

}  // namespace bf2
