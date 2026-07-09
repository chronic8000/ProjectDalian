#pragma once

#include "engine/core/level_loader.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"
#include "engine/physics/physics_world.hpp"

#include <string>
#include <vector>

namespace bf2 {

struct PlacementAuditEntry {
  std::string category;  // static | overgrowth | vehicle
  std::string name;
  float x = 0.f;
  float z = 0.f;
  float placed_y = 0.f;
  float terrain_y = 0.f;
  float mesh_foot_y = 0.f;  // world-space bottom of mesh AABB (approx)
  float gap = 0.f;          // mesh_foot_y - terrain_y (positive = floating)
  float mesh_min_y = 0.f;
};

struct PlacementAuditReport {
  std::string mod;
  std::string level;
  std::size_t total = 0;
  std::size_t float_count = 0;   // gap > threshold
  std::size_t embed_count = 0;   // gap < -threshold
  float float_threshold = 0.35f;
  float embed_threshold = 0.25f;
  std::vector<PlacementAuditEntry> outliers;
};

// Compare authored/scattered Y against terrain + mesh foot offset. Flags hover/sink outliers.
PlacementAuditReport audit_placement_heights(const std::string& bf2_root, const std::string& mod,
                                             const std::string& level_name,
                                             float float_threshold = 0.35f,
                                             float embed_threshold = 0.25f,
                                             std::size_t max_outliers = 200);

void log_placement_audit(const PlacementAuditReport& report, bool verbose = false);

}  // namespace bf2
