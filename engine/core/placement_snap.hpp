#pragma once

#include "engine/physics/physics_world.hpp"

#include <string>

namespace bf2 {

struct PlacementSnapResult {
  float y = 0.f;
  bool snapped = false;
};

// Rest mesh foot on terrain or collision surface (decks/roads). Skips large vertical offsets
// so rooftop props are not pulled to ground.
PlacementSnapResult snap_placement_y(PhysicsWorld& world, float x, float authored_y, float z,
                                   float mesh_min_y, float max_pull_down = 3.f,
                                   float max_float_fix = 1.25f);

bool is_foliage_template(const std::string& template_name);

}  // namespace bf2
