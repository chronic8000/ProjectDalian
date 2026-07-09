#pragma once

#include "engine/render/renderer.hpp"

namespace dalian {

inline bool ui_hit(const bf2::Renderer& r, int mx, int my, float x, float y, float w, float h) {
  float dx = 0.f, dy = 0.f;
  r.ui_unproject(mx, my, dx, dy);
  return dx >= x && dx <= x + w && dy >= y && dy <= y + h;
}

inline void ui_mouse_design(const bf2::Renderer& r, int mx, int my, float& dx, float& dy) {
  r.ui_unproject(mx, my, dx, dy);
}

}  // namespace dalian
