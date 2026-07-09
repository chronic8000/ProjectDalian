#include "minimap_projector.hpp"

#include <algorithm>
#include <cmath>

namespace dalian {

void MinimapProjector::configure(const glm::vec2& world_min_xz, const glm::vec2& world_max_xz,
                                 float screen_x, float screen_y, float screen_w, float screen_h) {
  world_min_xz_ = world_min_xz;
  world_max_xz_ = world_max_xz;
  screen_x_ = screen_x;
  screen_y_ = screen_y;
  screen_w_ = std::max(screen_w, 1e-6f);
  screen_h_ = std::max(screen_h, 1e-6f);
}

glm::vec2 MinimapProjector::world_to_minimap(const glm::vec3& world_pos) const {
  const float wx = world_max_xz_.x - world_min_xz_.x;
  const float wz = world_max_xz_.y - world_min_xz_.y;
  const float u = wx > 1e-6f ? (world_pos.x - world_min_xz_.x) / wx : 0.5f;
  const float v = wz > 1e-6f ? (world_pos.z - world_min_xz_.y) / wz : 0.5f;
  return {screen_x_ + u * screen_w_, screen_y_ + (1.f - v) * screen_h_};
}

float MinimapProjector::forward_to_arrow_angle(const glm::vec3& forward) const {
  // 0 radians = arrow points up on the minimap (north / -Z in typical BF2 layouts).
  return std::atan2(forward.x, -forward.z);
}

bool minimap_projector_self_test() {
  MinimapProjector proj;
  proj.configure({-512.f, -512.f}, {512.f, 512.f}, 100.f, 50.f, 200.f, 200.f);
  const glm::vec2 center = proj.world_to_minimap({0.f, 0.f, 0.f});
  if (std::fabs(center.x - 200.f) > 1e-3f || std::fabs(center.y - 150.f) > 1e-3f) return false;
  const glm::vec2 corner = proj.world_to_minimap({-512.f, 0.f, 512.f});
  if (std::fabs(corner.x - 100.f) > 1e-3f || std::fabs(corner.y - 50.f) > 1e-3f) return false;
  const float ang = proj.forward_to_arrow_angle({0.f, 0.f, -1.f});
  if (std::fabs(ang) > 1e-4f) return false;
  const float east = proj.forward_to_arrow_angle({1.f, 0.f, 0.f});
  if (std::fabs(east - 1.5707963f) > 1e-3f) return false;
  return true;
}

}  // namespace dalian
