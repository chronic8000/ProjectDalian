#pragma once

#include "engine/core/asset_audit.hpp"
#include "engine/core/level_loader.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"

#include <string>
#include <vector>

namespace bf2 {

struct LevelValidationResult {
  std::string mod;
  std::string level;
  std::string server_zip;
  bool load_ok = false;
  std::size_t placements = 0;
  std::size_t roads = 0;
  bool has_terrain = false;
  bool has_nav = false;
  bool has_overgrowth = false;
  bool has_undergrowth = false;
  bool has_sky = false;
  AssetAuditReport audit;
  std::string error;
};

// Mount archives and validate one level's server.zip (+ sibling client/objects).
LevelValidationResult validate_level(const std::string& bf2_root, const std::string& mod,
                                     const std::string& level_name);

struct ModLevelEntry {
  std::string mod;
  std::string level;
  std::string server_zip;
};

// Discover mods/*/Levels/*/server.zip under a BF2 install root.
std::vector<ModLevelEntry> discover_levels(const std::string& bf2_root);

}  // namespace bf2
