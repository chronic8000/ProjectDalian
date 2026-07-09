#include "engine/render/texture_cache.hpp"

#include "engine/render/texture_resolve.hpp"

namespace bf2 {

std::uint32_t TextureCache::get(const std::string& bf2_path, const std::string& mesh_folder) {
  if (bf2_path.empty()) {
    return 0;
  }
  const std::string key = bf2_path;
  const std::string cache_key =
      mesh_folder.empty() ? key : (key + "|" + mesh_folder);
  if (const auto it = cache_.find(cache_key); it != cache_.end()) {
    return it->second;
  }

  const auto candidates = texture_candidate_paths(bf2_path, mesh_folder);

  std::uint32_t id = 0;
  for (const auto& cand : candidates) {
    if (!resources_.archives().exists(cand)) {
      continue;
    }
    const auto bytes = resources_.archives().read(cand);
    if (!bytes) {
      continue;
    }
    try {
      const DdsTexture tex = DdsLoader::load_from_memory(*bytes);
      id = renderer_.upload_texture(tex);
      if (id != 0) {
        try {
          const DdsTexture rgba = DdsLoader::decode_to_rgba8(tex);
          const std::size_t px = rgba.pixels.size() / 4;
          std::size_t clear = 0;
          std::size_t soft = 0;
          for (std::size_t i = 0; i < px; ++i) {
            const std::uint8_t a = rgba.pixels[i * 4 + 3];
            if (a < 32) ++clear;
            if (a < 96) ++soft;
          }
          const double fc = px ? static_cast<double>(clear) / px : 0.0;
          const double fs = px ? static_cast<double>(soft) / px : 0.0;
          if (fc > 0.02 || fs > 0.06) {
            cutout_.insert(id);
          }
        } catch (...) {
        }
        break;
      }
    } catch (...) {
      id = 0;
      continue;
    }
  }

  if (id != 0) {
    ++loaded_;
  } else {
    ++missing_;
  }
  cache_[cache_key] = id;
  return id;
}

void TextureCache::clear() {
  for (auto& [path, id] : cache_) {
    if (id != 0) {
      renderer_.destroy_texture(id);
    }
  }
  cache_.clear();
  loaded_ = 0;
  missing_ = 0;
}

}  // namespace bf2
