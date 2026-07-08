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
