#include "radial_menu.hpp"

#include <algorithm>
#include <cmath>

namespace dalian {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

float normalize_angle(float a) {
  while (a < 0.f) a += kTwoPi;
  while (a >= kTwoPi) a -= kTwoPi;
  return a;
}

}  // namespace

void RadialMenu::set_slice_count(int count) { slice_count_ = std::max(count, 1); }

void RadialMenu::set_deadzone_radius(float radius) { deadzone_radius_ = std::max(radius, 0.f); }

void RadialMenu::set_start_angle_radians(float angle) { start_angle_ = angle; }

int RadialMenu::hover_index(float center_x, float center_y, float mouse_x, float mouse_y) const {
  const float dx = mouse_x - center_x;
  const float dy = mouse_y - center_y;
  const float dist = std::sqrt(dx * dx + dy * dy);
  if (dist < deadzone_radius_) return -1;

  // Screen Y grows downward; atan2(dx, -dy) gives 0 at top, increasing clockwise.
  const float angle = normalize_angle(std::atan2(dx, -dy) - start_angle_);
  const float slice = kTwoPi / static_cast<float>(slice_count_);
  int idx = static_cast<int>(angle / slice);
  if (idx < 0) idx = 0;
  if (idx >= slice_count_) idx = slice_count_ - 1;
  return idx;
}

bool radial_menu_self_test() {
  RadialMenu menu;
  menu.set_slice_count(6);
  menu.set_deadzone_radius(20.f);
  menu.set_start_angle_radians(0.f);

  if (menu.hover_index(100.f, 100.f, 100.f, 100.f) != -1) return false;  // deadzone
  if (menu.hover_index(100.f, 100.f, 100.f, 40.f) != 0) return false;   // top slice
  if (menu.hover_index(100.f, 100.f, 160.f, 100.f) != 1) return false;  // right slice
  if (menu.hover_index(100.f, 100.f, 100.f, 170.f) != 3) return false;  // bottom slice
  return true;
}

}  // namespace dalian
