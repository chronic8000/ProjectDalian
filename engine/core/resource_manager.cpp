#include "resource_manager.hpp"

#include "archive_path_resolve.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace bf2 {

Mesh ResourceManager::load_mesh_file(const std::string& path) {
  if (auto it = mesh_cache_.find(path); it != mesh_cache_.end()) {
    return it->second;
  }
  auto mesh = MeshLoader::load_from_file(path);
  mesh_cache_[path] = mesh;
  return mesh;
}

Mesh ResourceManager::load_mesh(const std::string& virtual_path) {
  if (auto it = mesh_cache_.find(virtual_path); it != mesh_cache_.end()) {
    return it->second;
  }

  const auto bytes = read_bytes(virtual_path);
  if (!bytes) {
    throw std::runtime_error("Mesh not found in archives: " + virtual_path);
  }

  MeshKind kind = MeshKind::Static;
  std::string lower = virtual_path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower.ends_with("bundledmesh")) {
    kind = MeshKind::Bundled;
  } else if (lower.ends_with("skinnedmesh")) {
    kind = MeshKind::Skinned;
  }

  auto mesh = MeshLoader::load_from_memory(*bytes, kind);
  mesh_cache_[virtual_path] = mesh;
  return mesh;
}

DdsTexture ResourceManager::load_texture(const std::string& virtual_path) {
  if (auto it = texture_cache_.find(virtual_path); it != texture_cache_.end()) {
    return it->second;
  }
  const auto bytes = read_bytes(virtual_path);
  if (!bytes) {
    throw std::runtime_error("Texture not found in archives: " + virtual_path);
  }
  auto texture = DdsLoader::load_from_memory(*bytes);
  texture_cache_[virtual_path] = texture;
  return texture;
}

std::optional<std::vector<std::uint8_t>> ResourceManager::read_bytes(const std::string& virtual_path) const {
  for (const auto& cand : archive_candidate_paths(virtual_path)) {
    if (auto bytes = archives_.read(cand)) return bytes;
  }
  return std::nullopt;
}

}  // namespace bf2
