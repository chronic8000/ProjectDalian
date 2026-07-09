#pragma once

#include "app_settings.hpp"
#include "key_bindings.hpp"

namespace bf2 {
class Renderer;
}

namespace dalian {

// BF2-style control binding list with rebind-on-click. Returns true if bindings changed.
bool draw_controls_options(bf2::Renderer& r, Settings& settings, int mx, int my, bool clicked,
                           float& scroll_y, int& rebind_action_index, bool& capture_key);

// Call from options menu event loop while capture_key is true.
bool handle_controls_key_capture(const SDL_Event& e, Settings& settings, int& rebind_action_index,
                                 bool& capture_key);

}  // namespace dalian
