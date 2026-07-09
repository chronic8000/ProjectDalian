#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "engine/core/resource_manager.hpp"
#include "engine/render/renderer.hpp"

namespace bf2 {

// Resolves BF2 material texture paths against mounted archives and uploads them
// to the GPU once, caching the resulting GL texture ids. Handles BF2's mixed
// path conventions (backslashes, optional leading "objects/").
class TextureCache {
public:
  TextureCache(ResourceManager& resources, Renderer& renderer)
      : resources_(resources), renderer_(renderer) {}
  ~TextureCache() { clear(); }

  // Returns a GL texture id for the given BF2 texture path, or 0 if it cannot be
  // found/loaded. Results (including failures) are cached.
  std::uint32_t get(const std::string& bf2_path);

  void clear();

  std::size_t loaded_count() const { return loaded_; }
  std::size_t missing_count() const { return missing_; }

  // True if the texture is an alpha *cutout* mask (leaves, fences, grates) rather
  // than an opaque or specular-in-alpha map. Detected at load from the fraction
  // of fully-transparent texels, so it works for any map's assets.
  bool is_cutout(std::uint32_t id) const { return id != 0 && cutout_.count(id) != 0; }

private:
  ResourceManager& resources_;
  Renderer& renderer_;
  std::unordered_map<std::string, std::uint32_t> cache_;
  std::unordered_set<std::uint32_t> cutout_;
  std::size_t loaded_ = 0;
  std::size_t missing_ = 0;
};

}  // namespace bf2
