#pragma once

#include <string>
#include <vector>

namespace bf2 {

struct CompiledRoadPlacement {
  std::string mesh_vpath;
  float position[3] = {0.f, 0.f, 0.f};
};

// Parse CompiledRoads.con (object.create + loadMesh + absoluteposition blocks).
std::vector<CompiledRoadPlacement> parse_compiled_roads(const std::string& script);

bool compiled_roads_parser_self_test();

}  // namespace bf2
