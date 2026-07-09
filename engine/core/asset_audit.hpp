#pragma once

#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"

#include <string>
#include <vector>

namespace bf2 {

struct TextureAuditMiss {
  std::string template_name;
  std::string mesh_vpath;
  std::string slot;
  std::string raw_path;
  std::vector<std::string> tried;
};

struct AssetAuditReport {
  std::size_t placement_count = 0;
  std::size_t unresolved_templates = 0;
  std::vector<std::string> unresolved_names;
  std::size_t unique_meshes = 0;
  std::size_t texture_misses = 0;
  std::vector<TextureAuditMiss> misses;
};

// Scan every resolved static template mesh and report texture path failures.
AssetAuditReport audit_static_assets(ResourceManager& resources,
                                     const TemplateResolver& resolver,
                                     const std::vector<std::string>& placement_templates);

void log_asset_audit(const AssetAuditReport& report, bool verbose);

}  // namespace bf2
