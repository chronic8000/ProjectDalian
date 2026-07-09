#include "placement_snap.hpp"

#include <algorithm>
#include <cctype>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool is_foliage_template(const std::string& template_name) {
  const std::string n = lower(template_name);
  if (n.find("fern") != std::string::npos) return true;
  if (n.find("bush") != std::string::npos) return true;
  if (n.find("shrub") != std::string::npos) return true;
  if (n.find("grass") != std::string::npos) return true;
  if (n.find("tree") != std::string::npos) return true;
  if (n.find("palm") != std::string::npos) return true;
  if (n.find("plant") != std::string::npos) return true;
  if (n.rfind("nc_", 0) == 0) return true;
  if (n.rfind("veg_", 0) == 0) return true;
  return false;
}

PlacementSnapResult snap_placement_y(PhysicsWorld& world, float x, float authored_y, float z,
                                     float mesh_min_y, float max_pull_down, float max_float_fix) {
  PlacementSnapResult r;
  r.y = authored_y;
  const float terr = world.terrain_height(x, z);
  const float foot = authored_y + mesh_min_y;
  const float float_gap = foot - terr;

  float surface = terr;
  const float probe_top = std::max(authored_y, terr) + 4.f;
  const auto hit = world.raycast({x, probe_top, z}, {0.f, -1.f, 0.f}, probe_top - terr + 8.f);
  if (hit.hit && hit.normal.y > 0.35f) {
    surface = std::max(surface, hit.point.y);
  } else {
    surface = std::max(surface, world.support_height(x, z, probe_top, 4.f));
  }

  const float target_origin = surface - mesh_min_y;
  const float delta = target_origin - authored_y;

  if (float_gap > 0.15f && float_gap <= max_float_fix) {
    r.y = target_origin;
    r.snapped = true;
    return r;
  }
  if (delta < 0.f && -delta <= max_pull_down && authored_y - surface > 0.05f) {
    r.y = target_origin;
    r.snapped = true;
    return r;
  }
  return r;
}

}  // namespace bf2
