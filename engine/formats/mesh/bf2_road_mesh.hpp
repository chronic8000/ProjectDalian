#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

// BF2 RoadCompiled (.mesh under Levels/*/Roads/*_compiled.mesh).
// Not a StaticMesh — custom world-road format used by RoadCompiledRenderer.
//
// Vertex stride 32 (matches RoadCompiled.fx APP2VS):
//   float3 Pos | float2 Tex0 | float2 Tex1 | float Alpha
// Tex0 = surface (CLAMP U across width, WRAP V along). Tex1 = tiling detail.
struct RoadCompiledMesh {
  std::uint32_t version = 0;
  Float3 start{};
  float length = 0.f;
  Float3 end{};
  Float3 misc{};
  std::vector<Float3> positions;  // local to `start`
  std::vector<float> u;           // Tex0.u — across width [0,1]
  std::vector<float> v;           // Tex0.v — along road (wraps)
  std::vector<float> u1;          // Tex1.u — detail across
  std::vector<float> v1;          // Tex1.v — detail along (shader uses *0.1)
  std::vector<float> alpha;       // edge/distance fade
  std::vector<std::uint16_t> indices;
};

bool is_road_compiled_bytes(const std::vector<std::uint8_t>& data);
RoadCompiledMesh load_road_compiled(const std::vector<std::uint8_t>& data);

// Build renderable mesh data (positions in local space; translate by `start` /
// CompiledRoads absoluteposition when instancing).
TexturedMeshData road_compiled_to_textured(const RoadCompiledMesh& road,
                                           const std::string& base_map = {},
                                           const std::string& detail_map = {});

// Local-space triangle soup for physics (same space as textured positions).
std::vector<Float3> road_compiled_collision_soup(const RoadCompiledMesh& road);

bool road_compiled_self_test();

}  // namespace bf2
