#pragma once

#include "app_settings.hpp"
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

// After SDL window size / display-mode changes, sync drawable pixels and GL viewport.
// Call this whenever apply_window_settings() runs (Options APPLY, Alt+Enter, F11).
inline void refresh_display(SDL_Window* window, bf2::Renderer& renderer, int& width, int& height) {
  SDL_PumpEvents();
  sync_drawable_size(window, width, height);
  renderer.set_viewport(width, height);
}

}  // namespace dalian
