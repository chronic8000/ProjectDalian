#pragma once

#include "engine/render/renderer.hpp"
#include "menu_music.hpp"

#include <SDL.h>
#include <cstdint>

namespace dalian {

struct Settings;

// BF2-style loading UI: progress while archives load, then a Ready gate before deploy.
class LoadingScreen {
 public:
  void start_music(const Settings& settings);
  // Pump OS messages + redraw. Safe to call often from long load loops — redraw
  // is throttled; events are always drained so Windows won't kill us as hung.
  void pump(SDL_Window* window, bf2::Renderer& renderer, float progress, const char* phase,
            const char* detail = nullptr);
  void wait_until_ready(SDL_Window* window, bf2::Renderer& renderer, const char* subtitle);
  void stop_music();

 private:
  bool draw_ready_button(bf2::Renderer& renderer, int mx, int my, bool hovered) const;

  void draw(SDL_Window* window, bf2::Renderer& renderer, float progress, const char* phase,
            const char* detail, bool show_ready, int mx, int my);

  MenuMusic music_;
  bool music_started_ = false;
  std::uint32_t last_draw_ms_ = 0;
};

}  // namespace dalian
