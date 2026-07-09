#pragma once

namespace dalian {

// Battlefield 2 Commo Rose-style radial menu hit testing.
class RadialMenu {
public:
  void set_slice_count(int count);
  void set_deadzone_radius(float radius);
  void set_start_angle_radians(float angle);  // angle of slice 0 center (default: top)

  int slice_count() const { return slice_count_; }
  float deadzone_radius() const { return deadzone_radius_; }

  // Returns slice index 0..N-1, or -1 when inside the deadzone.
  int hover_index(float center_x, float center_y, float mouse_x, float mouse_y) const;

private:
  int slice_count_ = 6;
  float deadzone_radius_ = 24.f;
  float start_angle_ = 0.f;  // rotation of slice 0 (0 = top at 12 o'clock)
};

bool radial_menu_self_test();

}  // namespace dalian
