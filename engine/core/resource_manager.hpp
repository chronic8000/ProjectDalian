#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "engine/formats/archive/archive.hpp"
#include "engine/formats/dds/dds_loader.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

class ResourceManager {
public:
  ArchiveMount& archives() { return archives_; }

  Mesh load_mesh(const std::string& virtual_path);
  Mesh load_mesh_file(const std::string& path);
  DdsTexture load_texture(const std::string& virtual_path);
  std::optional<std::vector<std::uint8_t>> read_bytes(const std::string& virtual_path) const;

private:
  ArchiveMount archives_;
  std::unordered_map<std::string, Mesh> mesh_cache_;
  std::unordered_map<std::string, DdsTexture> texture_cache_;
};

}  // namespace bf2
