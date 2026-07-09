#pragma once

#include "engine/render/renderer.hpp"
#include "menu_music.hpp"

#include <SDL.h>

namespace dalian {

struct Settings;

// BF2-style loading UI: progress while archives load, then a Ready gate before deploy.
class LoadingScreen {
 public:
  void start_music(const Settings& settings);
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
};

}  // namespace dalian
