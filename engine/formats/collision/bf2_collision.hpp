#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

struct CollisionFace {
  std::uint16_t v1 = 0;
  std::uint16_t v2 = 0;
  std::uint16_t v3 = 0;
  std::uint16_t material = 0;
};

// A single collision volume (col0..col3: projectile/vehicle/soldier/AI).
struct CollisionCol {
  std::uint32_t col_type = 0;
  std::vector<Float3> vertices;
  std::vector<std::uint16_t> vertex_materials;
  std::vector<CollisionFace> faces;
  Float3 bounds_min{};
  Float3 bounds_max{};
};

// Flattened view: every col across every geom/geom-part.
struct CollisionLod {
  std::uint32_t type = 0;
  std::vector<Float3> vertices;
  std::vector<CollisionFace> faces;
};

struct CollisionMesh {
  std::uint32_t version_major = 0;
  std::uint32_t version_minor = 0;
  std::vector<CollisionCol> cols;
  std::vector<CollisionLod> lods;  // alias of cols for backwards-compatible callers
};

class CollisionLoader {
public:
  static CollisionMesh load_from_memory(const std::vector<std::uint8_t>& data);
  static CollisionMesh load_from_file(const std::string& path);
};

}  // namespace bf2
