#include "archive_path_resolve.hpp"

#include <algorithm>
#include <cctype>

namespace bf2 {
namespace {

std::string normalize(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  std::transform(p.begin(), p.end(), p.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return p;
}

void push_unique(std::vector<std::string>& out, std::string p) {
  p = normalize(std::move(p));
  if (p.empty()) return;
  if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(std::move(p));
}

}  // namespace

std::vector<std::string> archive_candidate_paths(const std::string& virtual_path) {
  std::vector<std::string> candidates;
  if (virtual_path.empty()) return candidates;

  const std::string key = normalize(virtual_path);
  push_unique(candidates, key);

  // Levels/Dalian_plant/Roads/foo_compiled.mesh -> roads/foo_compiled.mesh
  if (key.rfind("levels/", 0) == 0) {
    const auto after_levels = key.find('/', 7);
    if (after_levels != std::string::npos) {
      const auto after_level = key.find('/', after_levels + 1);
      if (after_level != std::string::npos) {
        push_unique(candidates, key.substr(after_level + 1));
      }
    }
  }

  const auto slash = key.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string base = key.substr(slash + 1);
    if (base.find("_compiled.mesh") != std::string::npos) {
      push_unique(candidates, "roads/" + base);
    }
    push_unique(candidates, base);
  }

  return candidates;
}

std::string mesh_texture_folder_hint(const std::string& mesh_vpath) {
  const std::string key = normalize(mesh_vpath);
  const auto slash = key.find_last_of('/');
  if (slash == std::string::npos) return {};
  std::string folder = key.substr(0, slash);
  if (folder.rfind("levels/", 0) == 0) {
    const auto skip = folder.find('/', 7);
    if (skip != std::string::npos) folder = folder.substr(skip + 1);
  }
  return folder;
}

std::string resolve_mesh_vpath(const ArchiveMount& archives, const std::string& virtual_path) {
  for (const auto& cand : archive_candidate_paths(virtual_path)) {
    if (archives.exists(cand)) return cand;
  }
  return normalize(virtual_path);
}

bool archive_path_resolve_self_test() {
  const auto c = archive_candidate_paths("Levels/Dalian_plant/Roads/Mainroad_compiled.mesh");
  if (c.size() < 2) return false;
  if (c[0] != "levels/dalian_plant/roads/mainroad_compiled.mesh") return false;
  bool has_short = false;
  for (const auto& p : c) {
    if (p == "roads/mainroad_compiled.mesh") has_short = true;
  }
  if (!has_short) return false;
  if (mesh_texture_folder_hint("roads/mainroad_compiled.mesh") != "roads") return false;
  if (mesh_texture_folder_hint("levels/foo/roads/bar_compiled.mesh") != "roads") return false;
  return true;
}

}  // namespace bf2
