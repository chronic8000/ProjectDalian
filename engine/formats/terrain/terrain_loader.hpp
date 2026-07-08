#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

struct TerrainSample {
  float height = 0.f;
};

struct Terrain {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  float scale = 1.f;
  std::vector<TerrainSample> samples;
};

class TerrainLoader {
public:
  static Terrain load_raw_heightmap(const std::vector<std::uint8_t>& data, std::uint32_t width,
                                    std::uint32_t height, float scale = 1.f);
  static Terrain load_from_file(const std::string& path, std::uint32_t width, std::uint32_t height,
                                float scale = 1.f);
};

}  // namespace bf2
