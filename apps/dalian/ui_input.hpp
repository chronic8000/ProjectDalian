#pragma once

#include <SDL.h>

#include <cstring>
#include <cctype>

#include "engine/render/renderer.hpp"
#include "ui_layout.hpp"

namespace dalian {

// Simple single-line text field for menu UI (SDL keyboard input).
struct TextField {
  char buf[64] = {};
  int max_len = 63;
  bool focused = false;
  bool numeric_ip = false;  // if true, only digits and '.' are accepted
};

inline bool text_field_hit(const bf2::Renderer& r, int mx, int my, float x, float y, float w,
                           float h) {
  return ui_hit(r, mx, my, x, y, w, h);
}

inline void text_field_blur(TextField& field) {
  if (field.focused) {
    field.focused = false;
    SDL_StopTextInput();
  }
}

inline void text_field_focus(TextField& field) {
  if (!field.focused) {
    field.focused = true;
    SDL_StartTextInput();
  }
}

inline void text_field_handle_keydown(TextField& field, const SDL_KeyboardEvent& key) {
  if (!field.focused) return;
  if (key.keysym.sym == SDLK_BACKSPACE) {
    const int n = static_cast<int>(std::strlen(field.buf));
    if (n > 0) field.buf[n - 1] = '\0';
  } else if (key.keysym.sym == SDLK_ESCAPE || key.keysym.sym == SDLK_RETURN) {
    text_field_blur(field);
  }
}

inline void text_field_handle_text(TextField& field, const char* text) {
  if (!field.focused || !text) return;
  const int cur = static_cast<int>(std::strlen(field.buf));
  for (const char* p = text; *p && cur + static_cast<int>(p - text) < field.max_len; ++p) {
    const char c = *p;
    if (field.numeric_ip && !(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) continue;
    const int len = static_cast<int>(std::strlen(field.buf));
    if (len >= field.max_len) break;
    field.buf[len] = c;
    field.buf[len + 1] = '\0';
  }
}

// Draw the field; returns true if clicked this frame. Only one field should be
// focused at a time — pass `focus_group_blur` to blur others when this is clicked.
inline bool draw_text_field(bf2::Renderer& r, int mx, int my, bool clicked, float x, float y,
                            float w, float h, TextField& field, const char* placeholder,
                            TextField* focus_group_blur = nullptr) {
  const bool hov = text_field_hit(r, mx, my, x, y, w, h);
  if (clicked && hov) {
    if (focus_group_blur && focus_group_blur != &field) text_field_blur(*focus_group_blur);
    text_field_focus(field);
  } else if (clicked && field.focused && !hov) {
    text_field_blur(field);
  }
  r.ui_rect(x, y, w, h, field.focused ? 0.12f : 0.08f, field.focused ? 0.14f : 0.09f,
              field.focused ? 0.16f : 0.10f, 1.f);
  if (field.focused) {
    r.ui_rect(x, y, w, 2, 0.95f, 0.55f, 0.08f, 1.f);
  }
  const char* show = field.buf[0] ? field.buf : placeholder;
  const float alpha = field.buf[0] ? 0.92f : 0.45f;
  r.ui_text(x + 8, y + (h - 16) * 0.5f, 1.3f, show, alpha, alpha, alpha + 0.02f, 1.f);
  return clicked && hov;
}

}  // namespace dalian
