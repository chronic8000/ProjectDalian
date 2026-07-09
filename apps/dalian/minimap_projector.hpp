#pragma once

#include <glm/glm.hpp>

namespace dalian {

// Maps world XZ coordinates into a UI screen rectangle for minimap rendering.
class MinimapProjector {
public:
  void configure(const glm::vec2& world_min_xz, const glm::vec2& world_max_xz, float screen_x,
                 float screen_y, float screen_w, float screen_h);

  glm::vec2 world_to_minimap(const glm::vec3& world_pos) const;
  float forward_to_arrow_angle(const glm::vec3& forward) const;

  glm::vec2 world_min_xz() const { return world_min_xz_; }
  glm::vec2 world_max_xz() const { return world_max_xz_; }

private:
  glm::vec2 world_min_xz_{0.f};
  glm::vec2 world_max_xz_{1.f};
  float screen_x_ = 0.f;
  float screen_y_ = 0.f;
  float screen_w_ = 1.f;
  float screen_h_ = 1.f;
};

bool minimap_projector_self_test();

}  // namespace dalian
