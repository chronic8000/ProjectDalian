#include "ui_layout.hpp"

#include <algorithm>
#include <cmath>

namespace dalian {

float clamp_scroll(float scroll, float content_h, float viewport_h) {
  return std::clamp(scroll, 0.f, std::max(0.f, content_h - viewport_h));
}

std::string truncate_text(const bf2::Renderer& r, std::string text, float scale, float max_w) {
  if (max_w <= 1.f || text.empty()) return text;
  if (r.ui_text_width(text.c_str(), scale) <= max_w) return text;
  const char* ell = "...";
  const float ell_w = r.ui_text_width(ell, scale);
  if (ell_w >= max_w) return ell;
  while (text.size() > 1 &&
         r.ui_text_width((text + ell).c_str(), scale) > max_w) {
    text.pop_back();
  }
  return text + ell;
}

void draw_clipped_text(bf2::Renderer& r, float x, float y, float max_w, float scale,
                       const char* text, float cr, float cg, float cb, float a) {
  if (!text || !text[0]) return;
  const std::string clipped = truncate_text(r, text, scale, max_w);
  r.ui_text(x, y, scale, clipped.c_str(), cr, cg, cb, a);
}

}  // namespace dalian
