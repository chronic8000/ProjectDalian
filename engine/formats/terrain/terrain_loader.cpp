#include "terrain_loader.hpp"

#include <fstream>
#include <stdexcept>

namespace bf2 {

Terrain TerrainLoader::load_raw_heightmap(const std::vector<std::uint8_t>& data, std::uint32_t width,
                                          std::uint32_t height, float scale) {
  const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2;
  if (data.size() < expected) {
    throw std::runtime_error("Heightmap data too small");
  }

  Terrain terrain;
  terrain.width = width;
  terrain.height = height;
  terrain.scale = scale;
  terrain.samples.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

  for (std::size_t i = 0; i < terrain.samples.size(); ++i) {
    const std::uint16_t raw = static_cast<std::uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
    terrain.samples[i].height = static_cast<float>(raw) * scale;
  }

  return terrain;
}

Terrain TerrainLoader::load_from_file(const std::string& path, std::uint32_t width, std::uint32_t height,
                                      float scale) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Terrain file not found: " + path);
  }
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(data.data()), size);
  return load_raw_heightmap(data, width, height, scale);
}

}  // namespace bf2
