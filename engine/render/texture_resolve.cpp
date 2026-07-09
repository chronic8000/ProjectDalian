#include "texture_resolve.hpp"

#include <algorithm>
#include <cctype>

namespace bf2 {
namespace {

std::string normalize(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  std::transform(p.begin(), p.end(), p.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  return p;
}

std::string basename_of(std::string p) {
  p = normalize(std::move(p));
  const auto slash = p.find_last_of('/');
  if (slash != std::string::npos) p = p.substr(slash + 1);
  return p;
}

void push_unique(std::vector<std::string>& out, std::string p) {
  p = normalize(std::move(p));
  if (p.empty()) return;
  if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
}

}  // namespace

std::vector<std::string> texture_candidate_paths(const std::string& bf2_path,
                                                 const std::string& mesh_folder) {
  std::vector<std::string> candidates;
  if (bf2_path.empty()) return candidates;

  const std::string key = normalize(bf2_path);
  push_unique(candidates, key);
  if (key.rfind("objects/", 0) == 0) {
    push_unique(candidates, key.substr(std::string("objects/").size()));
  } else {
    push_unique(candidates, "objects/" + key);
  }
  // Meshes often reference .tga while archives store .dds.
  if (key.size() > 4 && key.rfind(".tga") == key.size() - 4) {
    std::string dds = key;
    dds.replace(dds.size() - 4, 4, ".dds");
    push_unique(candidates, dds);
    if (dds.rfind("objects/", 0) == 0) {
      push_unique(candidates, dds.substr(std::string("objects/").size()));
    } else {
      push_unique(candidates, "objects/" + dds);
    }
  }

  if (!mesh_folder.empty()) {
    const std::string folder = normalize(mesh_folder);
    const std::string base = basename_of(key);
    if (!base.empty()) {
      push_unique(candidates, folder + "/" + base);
      push_unique(candidates, folder + "/" + key);
      // BF2 object textures usually sit in the object root, not meshes/.
      const auto slash = folder.find_last_of('/');
      if (slash != std::string::npos && folder.substr(slash + 1) == "meshes") {
        push_unique(candidates, folder.substr(0, slash) + "/" + base);
        push_unique(candidates, folder.substr(0, slash) + "/" + key);
      }
    }
  }
  return candidates;
}

std::optional<std::string> resolve_texture_vpath(ResourceManager& resources,
                                                 const std::string& bf2_path,
                                                 const std::string& mesh_folder) {
  for (const auto& cand : texture_candidate_paths(bf2_path, mesh_folder)) {
    if (resources.archives().exists(cand)) return cand;
  }
  return std::nullopt;
}

}  // namespace bf2
