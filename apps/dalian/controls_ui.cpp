#include "controls_ui.hpp"

#include "ui_layout.hpp"

#include "engine/render/renderer.hpp"

#include <algorithm>
#include <cstdio>

namespace dalian {
namespace {

bool rect_hit(const bf2::Renderer& r, int mx, int my, float x, float y, float w, float h) {
  return ui_hit(r, mx, my, x, y, w, h);
}

bool draw_slider(bf2::Renderer& r, int mx, int my, bool clicked, float x, float y, float w,
                 const char* label, float& value, float vmin, float vmax) {
  r.ui_text(x, y, 1.3f, label, 0.75f, 0.78f, 0.82f, 1.f);
  const float sy = y + 22;
  r.ui_rect(x, sy, w, 8, 0.10f, 0.11f, 0.12f, 1.f);
  const float t = (value - vmin) / (vmax - vmin);
  r.ui_rect(x, sy, w * std::clamp(t, 0.f, 1.f), 8, 0.95f, 0.55f, 0.08f, 1.f);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f", value);
  r.ui_text(x + w + 12, sy - 4, 1.2f, buf, 0.95f, 0.55f, 0.08f, 1.f);
  if (clicked && rect_hit(r, mx, my, x, sy - 6, w, 20)) {
    float dx = 0.f, dy = 0.f;
    ui_mouse_design(r, mx, my, dx, dy);
    value = vmin + (vmax - vmin) * std::clamp((dx - x) / w, 0.f, 1.f);
    return true;
  }
  return false;
}

}  // namespace

bool draw_controls_options(bf2::Renderer& r, Settings& settings, int mx, int my, bool clicked,
                           float& scroll_y, int& rebind_action_index, bool& capture_key) {
  constexpr float ox = 60.f, oy = 150.f;
  constexpr float list_h = 580.f;
  bool changed = false;

  draw_clipped_text(r, ox, oy - 28, 880.f, 1.35f,
                    "CONTROLS  —  click a row to rebind. LMB/RMB = fire / zoom (BF2 default).",
                    0.75f, 0.78f, 0.82f, 1.f);

  if (draw_slider(r, mx, my, clicked, ox + 520.f, oy - 24, 220, "MOUSE SENSITIVITY",
                  settings.mouse_sensitivity, 0.02f, 0.4f)) {
    changed = true;
  }
  if (rect_hit(r, mx, my, ox + 520.f, oy + 4, 260, 22) && clicked) {
    settings.invert_mouse_y = !settings.invert_mouse_y;
    changed = true;
  }
  r.ui_text(ox + 520.f, oy + 6, 1.15f,
            settings.invert_mouse_y ? "[x] INVERT MOUSE Y" : "[ ] INVERT MOUSE Y", 0.8f, 0.82f,
            0.86f, 1.f);
  if (rect_hit(r, mx, my, ox + 520.f, oy + 28, 260, 22) && clicked) {
    settings.invert_air = !settings.invert_air;
    changed = true;
  }
  r.ui_text(ox + 520.f, oy + 30, 1.15f,
            settings.invert_air ? "[x] INVERT AIRCRAFT PITCH" : "[ ] INVERT AIRCRAFT PITCH", 0.8f,
            0.82f, 0.86f, 1.f);

  if (rect_hit(r, mx, my, ox + 760.f, oy - 24, 180, 28) && clicked) {
    settings.bindings.reset_bf2_defaults();
    changed = true;
  }
  r.ui_rect(ox + 760.f, oy - 24, 180, 28, 0.12f, 0.13f, 0.15f, 0.95f);
  r.ui_text(ox + 770.f, oy - 18, 1.15f, "RESET BF2 DEFAULTS", 0.95f, 0.55f, 0.08f, 1.f);

  r.ui_rect(ox, oy, 880.f, list_h, 0.05f, 0.06f, 0.07f, 0.92f);
  r.ui_text(ox + 8, oy + 6, 1.05f, "ACTION", 0.55f, 0.58f, 0.62f, 1.f);
  r.ui_text(ox + 420, oy + 6, 1.05f, "KEY", 0.55f, 0.58f, 0.62f, 1.f);
  r.ui_text(ox + 540, oy + 6, 1.05f, "STATUS", 0.55f, 0.58f, 0.62f, 1.f);
  r.ui_text(ox + 680, oy + 6, 1.05f, "CATEGORY", 0.55f, 0.58f, 0.62f, 1.f);

  const float row_h = 22.f;
  const float content_h = static_cast<float>(kGameActionCount) * row_h;
  scroll_y = std::clamp(scroll_y, 0.f, std::max(0.f, content_h - list_h + 28.f));

  float y = oy + 26.f - scroll_y;
  for (std::size_t i = 0; i < kGameActionCount; ++i) {
    const GameAction a = static_cast<GameAction>(i);
    const ActionMeta& m = KeyBindings::meta(a);
    if (y + row_h >= oy + 24.f && y <= oy + list_h) {
      const bool hov = rect_hit(r, mx, my, ox + 2, y, 876.f, row_h);
      if (hov) r.ui_rect(ox + 2, y, 876.f, row_h, 0.10f, 0.11f, 0.13f, 0.85f);
      const bool rebinding = static_cast<int>(i) == rebind_action_index;
      const SDL_Scancode sc = settings.bindings.scancode(a);
      char keybuf[48];
      if (rebinding)
        std::snprintf(keybuf, sizeof(keybuf), "Press a key...");
      else
        std::snprintf(keybuf, sizeof(keybuf), "%s",
                      KeyBindings::scancode_label(sc).c_str());
      const char* status = m.implemented ? "Ready" : "Coming soon";
      float sr = m.implemented ? 0.35f : 0.85f;
      float sg = m.implemented ? 0.85f : 0.55f;
      float sb = m.implemented ? 0.45f : 0.15f;
      if (!m.bf2_default) {
        sr = 0.55f;
        sg = 0.65f;
        sb = 0.95f;
        status = m.implemented ? "Dalian extra" : "Dalian stub";
      }
      draw_clipped_text(r, ox + 10, y + 3, 400.f, 1.05f, m.label, 0.88f, 0.90f, 0.93f, 1.f);
      draw_clipped_text(r, ox + 422, y + 3, 110.f, 1.05f, keybuf, 0.95f, 0.75f, 0.25f, 1.f);
      draw_clipped_text(r, ox + 542, y + 3, 130.f, 1.05f, status, sr, sg, sb, 1.f);
      draw_clipped_text(r, ox + 682, y + 3, 190.f, 1.0f, m.bf2_category, 0.6f, 0.63f, 0.66f, 1.f);
      if (clicked && hov && !capture_key) {
        rebind_action_index = static_cast<int>(i);
        capture_key = true;
      }
    }
    y += row_h;
  }

  r.ui_text(ox, oy + list_h + 8, 1.05f,
            "Dalian extras: F8 car SAM, F9/F10 drones, H medkit. Retail BF2 keys (G/Q/B/N/T/V) stay "
            "free for stubs.",
            0.55f, 0.58f, 0.62f, 1.f);

  return changed;
}

bool handle_controls_key_capture(const SDL_Event& e, Settings& settings, int& rebind_action_index,
                                 bool& capture_key) {
  if (!capture_key || rebind_action_index < 0) return false;
  if (e.type == SDL_KEYDOWN) {
    if (e.key.keysym.sym == SDLK_ESCAPE) {
      capture_key = false;
      rebind_action_index = -1;
      return true;
    }
    settings.bindings.load_scancode(static_cast<GameAction>(rebind_action_index),
                                    e.key.keysym.scancode);
    capture_key = false;
    rebind_action_index = -1;
    return true;
  }
  return false;
}

}  // namespace dalian
