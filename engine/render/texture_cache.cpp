#include "engine/render/texture_cache.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace bf2 {
namespace {

std::string normalize(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  std::transform(p.begin(), p.end(), p.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  return p;
}

}  // namespace

std::uint32_t TextureCache::get(const std::string& bf2_path) {
  if (bf2_path.empty()) {
    return 0;
  }
  const std::string key = normalize(bf2_path);
  if (const auto it = cache_.find(key); it != cache_.end()) {
    return it->second;
  }

  // Candidate virtual paths: as-is, without a leading "objects/", and with one.
  std::vector<std::string> candidates;
  candidates.push_back(key);
  if (key.rfind("objects/", 0) == 0) {
    candidates.push_back(key.substr(std::string("objects/").size()));
  } else {
    candidates.push_back("objects/" + key);
  }

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
        // Classify as a cutout mask if a meaningful fraction of texels are fully
        // transparent (typical of foliage/fence alpha). Specular-in-alpha maps
        // and opaque DXT1 have (almost) no alpha==0 texels, so they're excluded.
        try {
          const DdsTexture rgba = DdsLoader::decode_to_rgba8(tex);
          const std::size_t px = rgba.pixels.size() / 4;
          std::size_t clear = 0;  // fully transparent (hard cutout gaps)
          std::size_t soft = 0;   // semi-transparent (feathered foliage edges)
          for (std::size_t i = 0; i < px; ++i) {
            const std::uint8_t a = rgba.pixels[i * 4 + 3];
            if (a < 32) ++clear;
            if (a < 96) ++soft;
          }
          // Treat as a transparency mask if it has real holes, or a meaningful
          // band of soft alpha (leaf/plant/decal edges). Opaque building/fence
          // textures are DXT1 (alpha=255 everywhere), so they never qualify.
          const double fc = px ? static_cast<double>(clear) / px : 0.0;
          const double fs = px ? static_cast<double>(soft) / px : 0.0;
          if (fc > 0.02 || fs > 0.06) {
            cutout_.insert(id);
          }
        } catch (...) {
        }
      }
    } catch (...) {
      id = 0;
    }
    break;
  }

  if (id != 0) {
    ++loaded_;
  } else {
    ++missing_;
  }
  cache_[key] = id;
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
