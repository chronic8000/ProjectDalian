#pragma once

#include "key_bindings.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace dalian {

enum class FullscreenMode { Windowed = 0, Borderless = 1, Exclusive = 2 };

struct Settings {
  int width = 1600;
  int height = 900;
  FullscreenMode fullscreen = FullscreenMode::Windowed;
  bool vsync = true;
  bool show_fps = false;
  int msaa = 0;  // scene is offscreen FBO; window MSAA helps little — default off
  float fov = 74.f;
  float mouse_sensitivity = 0.12f;
  bool invert_air = false;
  // Remastered default: keep the full BF2 skyline (cranes, carrier, ridges) in view.
  float draw_distance = 8000.f;
  float fog_scale = 1.15f;
  // Internal 3D resolution vs window (1 = native). Biggest FPS lever on all GPUs.
  float render_scale = 0.85f;
  // 0=Bilinear, 1=FSR1 (EASU+RCAS), 2=Auto (→ FSR1 on OpenGL; DLSS/XeSS later).
  int upscale_mode = 1;
  float fsr_sharpness = 0.2f;  // RCAS stops: 0=max sharp, 2=soft
  // Positive = prefer cheaper/blurrier mips (helps HD texture packs).
  float mip_lod_bias = 0.35f;
  float shadow_distance = 1400.f;  // world metres covered by cascades
  bool grass_enabled = true;
  float grass_distance = 55.f;
  bool bloom = true;
  float bloom_intensity = 0.45f;
  bool hdr = false;              // off by default — can look washed/bright
  float hdr_exposure = 0.55f;    // only used when hdr is on
  bool ssao = false;             // expensive; off by default for playable FPS
  bool shadows_enabled = true;
  int shadow_res = 2048;         // 4096 is very heavy with remastered textures
  int anisotropic = 4;           // 8–16 is costly with remastered albedo
  float master_volume = 1.f;
  float sfx_volume = 1.f;
  float music_volume = 0.85f;
  float voice_volume = 1.f;
  bool invert_mouse_y = false;
  KeyBindings bindings;
  std::string bf2_root;
  std::string player_name = "Player";
  std::uint16_t net_port = 27015;
  bool use_tailscale = true;
  std::string tailscale_subnet;
  std::string manual_server_ip;
  bool allow_late_join = true;
  int default_faction = 0;
  bool mp_bots_enabled = true;
  int mp_bot_count = 28;
  int mp_bot_difficulty = 3;
  bool menu_music_enabled = true;
  std::string menu_music;

  static Settings load() {
    Settings s;
    s.apply_env_defaults();
    const std::string path = config_path();
    std::ifstream in(path);
    if (!in) return s;
    std::string line;
    while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = line.substr(0, eq);
      const std::string val = line.substr(eq + 1);
      if (key == "width") s.width = std::atoi(val.c_str());
      else if (key == "height") s.height = std::atoi(val.c_str());
      else if (key == "fullscreen") s.fullscreen = static_cast<FullscreenMode>(std::atoi(val.c_str()));
      else if (key == "vsync") s.vsync = val == "1" || val == "true";
      else if (key == "show_fps") s.show_fps = val == "1" || val == "true";
      else if (key == "msaa") s.msaa = std::atoi(val.c_str());
      else if (key == "fov") s.fov = static_cast<float>(std::atof(val.c_str()));
      else if (key == "mouse_sensitivity") s.mouse_sensitivity = static_cast<float>(std::atof(val.c_str()));
      else if (key == "invert_air") s.invert_air = val == "1" || val == "true";
      else if (key == "draw_distance")
        s.draw_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 500.f, 16000.f);
      else if (key == "fog_scale") s.fog_scale = static_cast<float>(std::atof(val.c_str()));
      else if (key == "render_scale")
        s.render_scale = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.5f, 1.f);
      else if (key == "upscale_mode") s.upscale_mode = std::clamp(std::atoi(val.c_str()), 0, 2);
      else if (key == "fsr_sharpness")
        s.fsr_sharpness = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, 2.f);
      else if (key == "mip_lod_bias")
        s.mip_lod_bias = std::clamp(static_cast<float>(std::atof(val.c_str())), -2.f, 2.f);
      else if (key == "shadow_distance")
        s.shadow_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 200.f, 4000.f);
      else if (key == "grass_enabled") s.grass_enabled = val == "1" || val == "true";
      else if (key == "grass_distance")
        s.grass_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 10.f, 200.f);
      else if (key == "bloom") s.bloom = val == "1" || val == "true";
      else if (key == "bloom_intensity") s.bloom_intensity = static_cast<float>(std::atof(val.c_str()));
      else if (key == "hdr") s.hdr = val == "1" || val == "true";
      else if (key == "hdr_exposure")
        s.hdr_exposure = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.15f, 2.5f);
      else if (key == "ssao") s.ssao = val == "1" || val == "true";
      else if (key == "shadows_enabled") s.shadows_enabled = val == "1" || val == "true";
      else if (key == "shadow_res") s.shadow_res = std::atoi(val.c_str());
      else if (key == "anisotropic") s.anisotropic = std::atoi(val.c_str());
      else if (key == "master_volume") s.master_volume = static_cast<float>(std::atof(val.c_str()));
      else if (key == "sfx_volume") s.sfx_volume = static_cast<float>(std::atof(val.c_str()));
      else if (key == "music_volume") s.music_volume = static_cast<float>(std::atof(val.c_str()));
      else if (key == "voice_volume") s.voice_volume = static_cast<float>(std::atof(val.c_str()));
      else if (key == "invert_mouse_y") s.invert_mouse_y = val == "1" || val == "true";
      else if (s.bindings.load_kv(key, val)) {
      }
      else if (key == "bf2_root") s.bf2_root = val;
      else if (key == "player_name") s.player_name = val;
      else if (key == "net_port") s.net_port = static_cast<std::uint16_t>(std::atoi(val.c_str()));
      else if (key == "use_tailscale") s.use_tailscale = val == "1" || val == "true";
      else if (key == "tailscale_subnet") s.tailscale_subnet = val;
      else if (key == "manual_server_ip") s.manual_server_ip = val;
      else if (key == "allow_late_join") s.allow_late_join = val == "1" || val == "true";
      else if (key == "default_faction") s.default_faction = std::atoi(val.c_str());
      else if (key == "mp_bots_enabled") s.mp_bots_enabled = val == "1" || val == "true";
      else if (key == "mp_bot_count") s.mp_bot_count = std::atoi(val.c_str());
      else if (key == "mp_bot_difficulty") s.mp_bot_difficulty = std::atoi(val.c_str());
      else if (key == "menu_music_enabled") s.menu_music_enabled = val == "1" || val == "true";
      else if (key == "menu_music") s.menu_music = val;
    }
    if (s.width < 640 || s.height < 480) {
      s.width = 1920;
      s.height = 1080;
    }
    // Migrate pre-remaster visibility defaults (old default was 2000 / fog 1.0).
    if (s.draw_distance > 0.f && s.draw_distance <= 2500.f) s.draw_distance = 8000.f;
    if (s.fog_scale > 0.99f && s.fog_scale < 1.01f) s.fog_scale = 1.15f;
    if (static_cast<int>(s.fullscreen) < 0 || static_cast<int>(s.fullscreen) > 2)
      s.fullscreen = FullscreenMode::Windowed;
    if (const char* wf = std::getenv("BF2_WINDOWED")) {
      if (wf[0] != '0' && wf[0] != 'n' && wf[0] != 'N')
        s.fullscreen = FullscreenMode::Windowed;
    }
    return s;
  }

  void save() const {
    const std::string path = config_path();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path);
    if (!out) return;
    out << "width=" << width << '\n';
    out << "height=" << height << '\n';
    out << "fullscreen=" << static_cast<int>(fullscreen) << '\n';
    out << "vsync=" << (vsync ? 1 : 0) << '\n';
    out << "show_fps=" << (show_fps ? 1 : 0) << '\n';
    out << "msaa=" << msaa << '\n';
    out << "fov=" << fov << '\n';
    out << "mouse_sensitivity=" << mouse_sensitivity << '\n';
    out << "invert_air=" << (invert_air ? 1 : 0) << '\n';
    out << "draw_distance=" << draw_distance << '\n';
    out << "fog_scale=" << fog_scale << '\n';
    out << "render_scale=" << render_scale << '\n';
    out << "upscale_mode=" << upscale_mode << '\n';
    out << "fsr_sharpness=" << fsr_sharpness << '\n';
    out << "mip_lod_bias=" << mip_lod_bias << '\n';
    out << "shadow_distance=" << shadow_distance << '\n';
    out << "grass_enabled=" << (grass_enabled ? 1 : 0) << '\n';
    out << "grass_distance=" << grass_distance << '\n';
    out << "bloom=" << (bloom ? 1 : 0) << '\n';
    out << "bloom_intensity=" << bloom_intensity << '\n';
    out << "hdr=" << (hdr ? 1 : 0) << '\n';
    out << "hdr_exposure=" << hdr_exposure << '\n';
    out << "ssao=" << (ssao ? 1 : 0) << '\n';
    out << "shadows_enabled=" << (shadows_enabled ? 1 : 0) << '\n';
    out << "shadow_res=" << shadow_res << '\n';
    out << "anisotropic=" << anisotropic << '\n';
    out << "master_volume=" << master_volume << '\n';
    out << "sfx_volume=" << sfx_volume << '\n';
    out << "music_volume=" << music_volume << '\n';
    out << "voice_volume=" << voice_volume << '\n';
    out << "invert_mouse_y=" << (invert_mouse_y ? 1 : 0) << '\n';
    bindings.save_all(out);
    out << "bf2_root=" << bf2_root << '\n';
    out << "player_name=" << player_name << '\n';
    out << "net_port=" << net_port << '\n';
    out << "use_tailscale=" << (use_tailscale ? 1 : 0) << '\n';
    out << "tailscale_subnet=" << tailscale_subnet << '\n';
    out << "manual_server_ip=" << manual_server_ip << '\n';
    out << "allow_late_join=" << (allow_late_join ? 1 : 0) << '\n';
    out << "default_faction=" << default_faction << '\n';
    out << "mp_bots_enabled=" << (mp_bots_enabled ? 1 : 0) << '\n';
    out << "mp_bot_count=" << mp_bot_count << '\n';
    out << "mp_bot_difficulty=" << mp_bot_difficulty << '\n';
    out << "menu_music_enabled=" << (menu_music_enabled ? 1 : 0) << '\n';
    out << "menu_music=" << menu_music << '\n';
  }

  void apply_env_defaults() {
    if (const char* dd = std::getenv("BF2_DRAWDIST"))
      draw_distance = std::clamp(static_cast<float>(std::atof(dd)), 500.f, 16000.f);
    if (const char* fs = std::getenv("BF2_FOGSCALE"))
      fog_scale = std::max(0.05f, static_cast<float>(std::atof(fs)));
    if (const char* ms = std::getenv("BF2_MSAA")) msaa = std::atoi(ms);
    if (const char* b = std::getenv("BF2_BLOOM"))
      bloom = !(b[0] == '0' || b[0] == 'n' || b[0] == 'N');
    if (const char* bi = std::getenv("BF2_BLOOMI"))
      bloom_intensity = static_cast<float>(std::atof(bi));
    if (const char* h = std::getenv("BF2_HDR"))
      hdr = !(h[0] == '0' || h[0] == 'n' || h[0] == 'N');
    if (const char* he = std::getenv("BF2_HDR_EXPOSURE"))
      hdr_exposure = std::clamp(static_cast<float>(std::atof(he)), 0.15f, 2.5f);
    if (const char* ao = std::getenv("BF2_SSAO"))
      ssao = !(ao[0] == '0' || ao[0] == 'n' || ao[0] == 'N');
    if (const char* ia = std::getenv("BF2_INVERTAIR"))
      invert_air = ia[0] == '1' || ia[0] == 't' || ia[0] == 'T' || ia[0] == 'y' ||
                   ia[0] == 'Y' || ia[0] == 'o' || ia[0] == 'O';
  }

  static std::string config_path() {
    char* base = SDL_GetPrefPath("ProjectDalian", "ProjectDalian");
    std::string path = base ? std::string(base) + "settings.cfg" : "settings.cfg";
    if (base) SDL_free(base);
    return path;
  }
};

inline void apply_window_settings(SDL_Window* window, Settings& s);

inline std::pair<int, int> query_drawable_size(SDL_Window* window) {
  int w = 0, h = 0;
  if (window) {
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w <= 0 || h <= 0) SDL_GetWindowSize(window, &w, &h);
  }
  if (w <= 0) w = 1600;
  if (h <= 0) h = 900;
  return {w, h};
}

inline void sync_drawable_size(SDL_Window* window, int& width, int& height) {
  const auto [dw, dh] = query_drawable_size(window);
  width = dw;
  height = dh;
}

inline void force_windowed_recovery(SDL_Window* window, Settings& s) {
  s.fullscreen = FullscreenMode::Windowed;
  s.width = 1920;
  s.height = 1080;
  apply_window_settings(window, s);
  s.save();
}

// Alt+Enter toggles windowed/borderless; F11 cycles all three modes;
// Ctrl+Shift+W forces safe windowed 1920x1080 (recovery from broken exclusive FS).
inline bool handle_display_hotkey(SDL_Window* window, Settings& s, const SDL_Event& e) {
  if (e.type != SDL_KEYDOWN) return false;
  const Uint16 mod = e.key.keysym.mod;
  const bool ctrl = (mod & KMOD_CTRL) != 0;
  const bool shift = (mod & KMOD_SHIFT) != 0;
  const bool alt = (mod & KMOD_ALT) != 0;
  if (ctrl && shift && e.key.keysym.sym == SDLK_w) {
    force_windowed_recovery(window, s);
    return true;
  }
  if (e.key.keysym.sym == SDLK_RETURN && alt) {
    if (s.fullscreen == FullscreenMode::Windowed)
      s.fullscreen = FullscreenMode::Borderless;
    else
      s.fullscreen = FullscreenMode::Windowed;
    apply_window_settings(window, s);
    s.save();
    return true;
  }
  if (e.key.keysym.sym == SDLK_F11) {
    const int next = (static_cast<int>(s.fullscreen) + 1) % 3;
    s.fullscreen = static_cast<FullscreenMode>(next);
    apply_window_settings(window, s);
    s.save();
    return true;
  }
  return false;
}

inline void apply_window_settings(SDL_Window* window, Settings& s) {
  const int display = SDL_GetWindowDisplayIndex(window);
  if (display < 0) return;

  // Leave fullscreen before changing size or display mode (required on Windows).
  SDL_SetWindowFullscreen(window, 0);

  if (s.fullscreen == FullscreenMode::Windowed) {
    SDL_SetWindowDisplayMode(window, nullptr);
    SDL_SetWindowSize(window, s.width, s.height);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  } else if (s.fullscreen == FullscreenMode::Borderless) {
    SDL_SetWindowDisplayMode(window, nullptr);
    SDL_DisplayMode desktop{};
    if (SDL_GetDesktopDisplayMode(display, &desktop) == 0) {
      s.width = desktop.w;
      s.height = desktop.h;
    }
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  } else {
    SDL_DisplayMode want{};
    want.w = s.width;
    want.h = s.height;
    want.refresh_rate = 0;
    want.format = 0;
    SDL_DisplayMode closest{};
    if (SDL_GetClosestDisplayMode(display, &want, &closest) == 0) {
      if (SDL_SetWindowDisplayMode(window, &closest) != 0) {
        std::cerr << "Display mode " << s.width << 'x' << s.height << " unavailable: "
                  << SDL_GetError() << " — using closest " << closest.w << 'x' << closest.h
                  << '\n';
      }
      s.width = closest.w;
      s.height = closest.h;
    } else {
      std::cerr << "No matching exclusive display mode for " << s.width << 'x' << s.height
                << ", staying windowed\n";
      s.fullscreen = FullscreenMode::Windowed;
      SDL_SetWindowDisplayMode(window, nullptr);
      SDL_SetWindowSize(window, s.width, s.height);
      SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (s.fullscreen == FullscreenMode::Exclusive)
      SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
  }

  SDL_GL_SetSwapInterval(s.vsync ? 1 : 0);
  SDL_PumpEvents();
  sync_drawable_size(window, s.width, s.height);
}

}  // namespace dalian
