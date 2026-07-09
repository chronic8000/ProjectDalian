#pragma once

#include "engine/core/overgrowth_parser.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/script/con_interpreter.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

// One scattered tree/bush from Overgrowth.raw + Overgrowth.con.
struct OvergrowthInstance {
  std::string mesh_vpath;
  float position[3] = {0.f, 0.f, 0.f};
  float rotation[3] = {0.f, 0.f, 0.f};
  float scale = 1.f;
};

// Resolve a geometry basename (e.g. nc_birch_cluster01) to an archive mesh path.
std::string resolve_overgrowth_mesh(ResourceManager& resources, const std::string& geometry);

// Scatter foliage instances from the 1025² (or N²) material-id grid in Overgrowth.raw.
// Uses the same world layout as Undergrowth (centered, cell_size metres per texel).
std::vector<OvergrowthInstance> build_overgrowth_instances(const OvergrowthParser& defs,
                                                           const std::vector<std::uint8_t>& raw,
                                                           ResourceManager& resources,
                                                           float cell_size = 2.f,
                                                           std::size_t max_instances = 80000);

bool overgrowth_instances_self_test();

}  // namespace bf2
