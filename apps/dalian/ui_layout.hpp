#pragma once

#include "app_settings.hpp"
#include "engine/render/renderer.hpp"

#include <string>

namespace dalian {

inline bool ui_hit(const bf2::Renderer& r, int mx, int my, float x, float y, float w, float h) {
  float dx = 0.f, dy = 0.f;
  r.ui_unproject(mx, my, dx, dy);
  return dx >= x && dx <= x + w && dy >= y && dy <= y + h;
}

inline void ui_mouse_design(const bf2::Renderer& r, int mx, int my, float& dx, float& dy) {
  r.ui_unproject(mx, my, dx, dy);
}

inline void refresh_display(SDL_Window* window, bf2::Renderer& renderer, int& width, int& height) {
  SDL_PumpEvents();
  sync_drawable_size(window, width, height);
  renderer.set_viewport(width, height);
}

// Scroll offset clamped so list content stays within viewport.
float clamp_scroll(float scroll, float content_h, float viewport_h);

// Ellipsize text to fit max_w at the given UI scale.
std::string truncate_text(const bf2::Renderer& r, std::string text, float scale, float max_w);

// Draw text clipped to a column width (never spills horizontally).
void draw_clipped_text(bf2::Renderer& r, float x, float y, float max_w, float scale,
                       const char* text, float cr, float cg, float cb, float a);

}  // namespace dalian
