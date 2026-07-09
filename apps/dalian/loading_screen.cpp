#include "loading_screen.hpp"

#include "app_settings.hpp"
#include "ui_layout.hpp"

#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dalian {
namespace {

bool rect_hit(const bf2::Renderer& r, int mx, int my, float x, float y, float w, float h) {
  return ui_hit(r, mx, my, x, y, w, h);
}

}  // namespace

void LoadingScreen::start_music(const Settings& settings) {
  if (music_started_) return;
  if (!settings.menu_music_enabled) return;
  if (!music_.init()) return;
  const std::string track = resolve_loading_music_path(settings);
  if (track.empty()) {
    std::cerr << "LoadingMusic: Before_the_First_Volley.mp3 not found (check Downloads or music/ "
                 "folder)\n";
    return;
  }
  const float vol = std::clamp(settings.master_volume * settings.music_volume, 0.f, 1.f);
  if (music_.play_file(track, vol, 0)) music_started_ = true;
}

void LoadingScreen::stop_music() {
  if (!music_started_) return;
  music_.stop();
  music_.shutdown();
  music_started_ = false;
}

bool LoadingScreen::draw_ready_button(bf2::Renderer& renderer, int mx, int my, bool hovered) const {
  constexpr float W = 1600.f, H = 900.f;
  const float bw = 320.f, bh = 64.f;
  const float bx = (W - bw) * 0.5f;
  const float by = H * 0.55f;
  const bool hov = hovered && rect_hit(renderer, mx, my, bx, by, bw, bh);
  renderer.ui_rect(bx, by, bw, bh, hov ? 0.85f : 0.65f, hov ? 0.45f : 0.32f, hov ? 0.05f : 0.04f,
                   1.f);
  const float tw = renderer.ui_text_width("READY", 2.8f);
  renderer.ui_text(bx + (bw - tw) * 0.5f, by + 18.f, 2.8f, "READY", 0.05f, 0.05f, 0.06f, 1.f);
  return hov;
}

void LoadingScreen::draw(SDL_Window* window, bf2::Renderer& renderer, float progress,
                         const char* phase, const char* detail, bool show_ready, int mx, int my) {
  progress = std::clamp(progress, 0.f, 1.f);
  int sw = 0, sh = 0;
  sync_drawable_size(window, sw, sh);
  renderer.set_viewport(sw, sh);
  renderer.begin_frame(0.04f, 0.05f, 0.06f);
  renderer.begin_ui(window);

  constexpr float W = 1600.f, H = 900.f;
  renderer.ui_rect(0, 0, W, H, 0.03f, 0.04f, 0.05f, 1.f);
  renderer.ui_rect(0, 0, W, 4.f, 0.95f, 0.55f, 0.08f, 1.f);

  const float panel_x = W - 520.f, panel_y = 80.f, panel_w = 440.f, panel_h = H - 280.f;
  renderer.ui_rect(panel_x, panel_y, panel_w, panel_h, 0.06f, 0.07f, 0.09f, 1.f);
  renderer.ui_rect(panel_x, panel_y, panel_w, 3.f, 0.95f, 0.55f, 0.08f, 0.85f);
  renderer.ui_text(panel_x + 20.f, panel_y + 16.f, 1.25f, "DALIAN PLANT", 0.55f, 0.57f, 0.62f, 1.f);
  renderer.ui_text(panel_x + 20.f, panel_y + 44.f, 1.05f,
                   "Before the First Volley plays while assets load.", 0.45f, 0.48f, 0.52f, 1.f);
  renderer.ui_text(panel_x + 20.f, panel_y + 68.f, 1.05f, "Press Ready when you want to deploy.",
                   0.45f, 0.48f, 0.52f, 1.f);

  const char* headline = show_ready ? "READY" : "LOADING";
  renderer.ui_text(80, 80, 3.2f, headline, 0.95f, 0.96f, 0.98f, 1.f);
  if (phase && phase[0]) {
    renderer.ui_text(80, 130, 2.0f, phase, 0.95f, 0.55f, 0.08f, 1.f);
  }
  if (detail && detail[0]) {
    renderer.ui_text(80, 168, 1.35f, detail, 0.65f, 0.68f, 0.72f, 1.f);
  }

  const float bar_x = 80.f, bar_y = H - 160.f, bar_w = panel_x - bar_x - 40.f, bar_h = 22.f;
  renderer.ui_rect(bar_x, bar_y, bar_w, bar_h, 0.08f, 0.09f, 0.10f, 1.f);
  renderer.ui_rect(bar_x, bar_y, bar_w * progress, bar_h, 0.95f, 0.55f, 0.08f, 1.f);
  char pct[16];
  std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(progress * 100.f + 0.5f));
  renderer.ui_text(bar_x + bar_w - 60.f, bar_y - 28.f, 1.6f, pct, 0.85f, 0.88f, 0.90f, 1.f);

  if (show_ready) {
    draw_ready_button(renderer, mx, my, true);
    renderer.ui_text(80, H - 110, 1.15f,
                     "Click READY or press Enter / Space — music stops when you continue",
                     0.45f, 0.48f, 0.52f, 1.f);
  } else {
    renderer.ui_text(80, H - 110, 1.15f, "Loading level data — please wait...", 0.45f, 0.48f, 0.52f,
                     1.f);
  }

  renderer.end_ui();
  renderer.end_frame();
  SDL_GL_SwapWindow(window);
}

void LoadingScreen::pump(SDL_Window* window, bf2::Renderer& renderer, float progress,
                         const char* phase, const char* detail) {
  int mx = 0, my = 0;
  SDL_GetMouseState(&mx, &my);
  draw(window, renderer, progress, phase, detail, false, mx, my);

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) std::exit(0);
  }
}

void LoadingScreen::wait_until_ready(SDL_Window* window, bf2::Renderer& renderer,
                                     const char* subtitle) {
  if (std::getenv("BF2_SKIP_LOADING_READY")) {
    stop_music();
    return;
  }

  SDL_ShowCursor(SDL_ENABLE);
  bool ready = false;
  while (!ready) {
    SDL_Event e;
    bool clicked = false;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) std::exit(0);
      if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) clicked = true;
      if (e.type == SDL_KEYDOWN &&
          (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE ||
           e.key.keysym.sym == SDLK_KP_ENTER)) {
        ready = true;
      }
    }

    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    draw(window, renderer, 1.f, "MAP LOADED", subtitle, true, mx, my);

    constexpr float W = 1600.f, H = 900.f;
    const float bw = 320.f, bh = 64.f;
    const float bx = (W - bw) * 0.5f;
    const float by = H * 0.55f;
    if (clicked && rect_hit(renderer, mx, my, bx, by, bw, bh)) ready = true;
  }

  stop_music();
}

}  // namespace dalian
