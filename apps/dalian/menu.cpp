#include "menu.hpp"

#include "controls_ui.hpp"
#include "menu_background.hpp"
#include "menu_music.hpp"
#include "multiplayer_menu.hpp"
#include "ui_layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <vector>

namespace dalian {
namespace {

bool rect_hit(const bf2::Renderer& r, int mx, int my, float x, float y, float w, float h) {
  return ui_hit(r, mx, my, x, y, w, h);
}

std::string prettify_name(const std::string& folder) {
  std::string out;
  out.reserve(folder.size());
  bool cap = true;
  for (char c : folder) {
    if (c == '_') {
      out.push_back(' ');
      cap = true;
    } else {
      if (cap && std::isalpha(static_cast<unsigned char>(c))) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        cap = false;
      } else {
        out.push_back(c);
      }
    }
  }
  return out;
}

enum class TopTab { Play, Options, Multiplayer, Quit };
enum class OptTab { Video, Graphics, Controls, Audio };

struct UiTheme {
  static constexpr float kOrangeR = 0.95f, kOrangeG = 0.55f, kOrangeB = 0.08f;
  static constexpr float kBgR = 0.04f, kBgG = 0.05f, kBgB = 0.06f;
};

void draw_tab(bf2::Renderer& r, float x, float y, float w, float h, const char* label, bool sel,
              bool hov) {
  if (sel) {
    r.ui_rect(x, y, w, h, UiTheme::kOrangeR, UiTheme::kOrangeG, UiTheme::kOrangeB, 1.f);
    r.ui_text(x + 14, y + 10, 1.5f, label, 0.05f, 0.05f, 0.06f, 1.f);
  } else {
    r.ui_rect(x, y, w, h, 0.08f, 0.09f, 0.10f, hov ? 0.95f : 0.75f);
    r.ui_text(x + 14, y + 10, 1.5f, label, UiTheme::kOrangeR, UiTheme::kOrangeG,
              UiTheme::kOrangeB, 1.f);
  }
}

bool draw_button(bf2::Renderer& r, int mx, int my, float x, float y, float w, float h,
                 const char* label, bool primary = false) {
  const bool hov = rect_hit(r, mx, my, x, y, w, h);
  if (primary) {
    r.ui_rect(x, y, w, h, hov ? 0.85f : 0.65f, hov ? 0.45f : 0.32f, hov ? 0.05f : 0.04f, 1.f);
    const float tw = r.ui_text_width(label, 1.8f);
    r.ui_text(x + (w - tw) * 0.5f, y + (h - 18) * 0.5f, 1.8f, label, 0.05f, 0.05f, 0.06f, 1.f);
  } else {
    r.ui_rect(x, y, w, h, hov ? 0.14f : 0.10f, hov ? 0.12f : 0.09f, hov ? 0.11f : 0.08f, 1.f);
    const float tw = r.ui_text_width(label, 1.6f);
    r.ui_text(x + (w - tw) * 0.5f, y + (h - 16) * 0.5f, 1.6f, label, 0.9f, 0.92f, 0.94f, 1.f);
  }
  return hov;
}

bool draw_checkbox(bf2::Renderer& r, int mx, int my, float x, float y, const char* label,
                   bool checked) {
  const float bs = 18.f;
  const float label_w = r.ui_text_width(label, 1.4f);
  const float hit_w = std::min(420.f, bs + 14.f + label_w);
  const bool hov = rect_hit(r, mx, my, x, y, hit_w, bs + 4);
  r.ui_rect(x, y, bs, bs, 0.08f, 0.09f, 0.10f, 1.f);
  if (checked) {
    r.ui_rect(x + 3, y + 3, bs - 6, bs - 6, UiTheme::kOrangeR, UiTheme::kOrangeG,
              UiTheme::kOrangeB, 1.f);
  }
  draw_clipped_text(r, x + bs + 10, y + 2, 380.f, 1.4f, label, hov ? 1.f : 0.85f,
                    hov ? 0.88f : 0.82f, hov ? 0.90f : 0.84f, 1.f);
  return hov;
}

bool draw_slider(bf2::Renderer& r, int mx, int my, bool clicked, float x, float y, float w,
                 const char* label, float& value, float vmin, float vmax) {
  draw_clipped_text(r, x, y, w + 80.f, 1.3f, label, 0.75f, 0.78f, 0.82f, 1.f);
  const float sy = y + 22;
  r.ui_rect(x, sy, w, 8, 0.10f, 0.11f, 0.12f, 1.f);
  const float t = (value - vmin) / (vmax - vmin);
  r.ui_rect(x, sy, w * std::clamp(t, 0.f, 1.f), 8, UiTheme::kOrangeR, UiTheme::kOrangeG,
            UiTheme::kOrangeB, 1.f);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.0f", value);
  const float vw = r.ui_text_width(buf, 1.2f);
  r.ui_text(x + w - vw, sy - 4, 1.2f, buf, UiTheme::kOrangeR, UiTheme::kOrangeG,
            UiTheme::kOrangeB, 1.f);
  if (clicked && rect_hit(r, mx, my, x, sy - 6, w, 20)) {
    float dx = 0.f, dy = 0.f;
    ui_mouse_design(r, mx, my, dx, dy);
    value = vmin + (vmax - vmin) * std::clamp((dx - x) / w, 0.f, 1.f);
    return true;
  }
  return false;
}

struct DisplayModeEntry {
  int w = 0;
  int h = 0;
  std::string label;
};

std::vector<DisplayModeEntry> query_display_modes(SDL_Window* window) {
  std::vector<DisplayModeEntry> out;
  const int idx = SDL_GetWindowDisplayIndex(window);
  if (idx < 0) {
    out.push_back({1920, 1080, "1920 x 1080"});
    return out;
  }
  int desktop_w = 1920, desktop_h = 1080;
  if (SDL_DisplayMode desktop{}; SDL_GetDesktopDisplayMode(idx, &desktop) == 0) {
    desktop_w = desktop.w;
    desktop_h = desktop.h;
  }
  std::vector<std::pair<int, int>> modes;
  for (int i = 0; i < SDL_GetNumDisplayModes(idx); ++i) {
    SDL_DisplayMode m{};
    if (SDL_GetDisplayMode(idx, i, &m) != 0) continue;
    if (m.w < 800 || m.h < 600) continue;
    modes.emplace_back(m.w, m.h);
  }
  std::sort(modes.begin(), modes.end(), [](const auto& a, const auto& b) {
    return a.first * a.second > b.first * b.second;
  });
  modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
  bool have_desktop = false;
  for (const auto& [w, h] : modes) {
    DisplayModeEntry e;
    e.w = w;
    e.h = h;
    if (w == desktop_w && h == desktop_h) {
      e.label = std::to_string(w) + " x " + std::to_string(h) + "  (Desktop)";
      have_desktop = true;
    } else {
      e.label = std::to_string(w) + " x " + std::to_string(h);
    }
    out.push_back(std::move(e));
  }
  if (!have_desktop) {
    out.insert(out.begin(), {desktop_w, desktop_h,
                             std::to_string(desktop_w) + " x " + std::to_string(desktop_h) +
                                 "  (Desktop)"});
  }
  if (out.empty()) out.push_back({1920, 1080, "1920 x 1080"});
  return out;
}

int find_resolution_index(const std::vector<DisplayModeEntry>& modes, int w, int h) {
  for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
    if (modes[i].w == w && modes[i].h == h) return i;
  }
  int best = 0;
  long long best_diff = 1LL << 62;
  for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
    const long long diff =
        std::llabs(static_cast<long long>(modes[i].w - w)) +
        std::llabs(static_cast<long long>(modes[i].h - h));
    if (diff < best_diff) {
      best_diff = diff;
      best = i;
    }
  }
  return best;
}

struct OptionPopupAnchors {
  float res_x = 0.f, res_y = 0.f;
  float fs_x = 0.f, fs_y = 0.f;
  float msaa_x = 0.f, msaa_y = 0.f;
  float shadow_x = 0.f, shadow_y = 0.f;
};

void draw_options_content(bf2::Renderer& r, Settings& settings, OptTab tab, int mx, int my,
                          bool clicked, int& res_idx, bool& res_open, int& fs_idx, bool& fs_open,
                          int& msaa_idx, bool& msaa_open, int& shadow_idx, bool& shadow_open,
                          const std::vector<DisplayModeEntry>& modes, OptionPopupAnchors& anchors,
                          float& controls_scroll, float& graphics_scroll, int& rebind_action,
                          bool& capture_key) {
  constexpr float H = 900.f;
  const float ox = 48, oy = 160;
  if (tab == OptTab::Video) {
    r.ui_text(ox, oy, 1.5f, "VIDEO SETTINGS", UiTheme::kOrangeR, UiTheme::kOrangeG,
              UiTheme::kOrangeB, 1.f);
    float y = oy + 36;
    r.ui_text(ox, y, 1.3f, "RESOLUTION", 0.75f, 0.78f, 0.82f, 1.f);
    y += 22;
    const bool borderless = fs_idx == static_cast<int>(FullscreenMode::Borderless);
    res_idx = std::clamp(res_idx, 0, static_cast<int>(modes.size()) - 1);
    const char* rlabel =
        borderless ? "Desktop (borderless)" : modes[static_cast<std::size_t>(res_idx)].label.c_str();
    anchors.res_x = ox;
    anchors.res_y = y;
    if (!borderless && draw_button(r, mx, my, ox, y, 280, 32, rlabel)) {
      if (clicked) res_open = !res_open;
    } else if (borderless) {
      r.ui_rect(ox, y, 280, 32, 0.08f, 0.09f, 0.10f, 0.75f);
      draw_clipped_text(r, ox + 12, y + 8, 256.f, 1.3f, rlabel, 0.55f, 0.58f, 0.62f, 1.f);
      res_open = false;
    }
    y += 50;
    const char* fs_labels[] = {"WINDOWED", "BORDERLESS", "EXCLUSIVE"};
    r.ui_text(ox, y, 1.3f, "FULLSCREEN", 0.75f, 0.78f, 0.82f, 1.f);
    y += 22;
    anchors.fs_x = ox;
    anchors.fs_y = y;
    if (draw_button(r, mx, my, ox, y, 280, 32, fs_labels[fs_idx])) {
      if (clicked) fs_open = !fs_open;
    }
    y += 50;
    if (draw_checkbox(r, mx, my, ox, y, "VERTICAL SYNC", settings.vsync) && clicked)
      settings.vsync = !settings.vsync;
    y += 32;
    if (draw_checkbox(r, mx, my, ox, y, "SHOW FPS", settings.show_fps) && clicked)
      settings.show_fps = !settings.show_fps;
    y += 32;
    r.ui_text(ox, y, 1.3f, "ANTI-ALIASING (MSAA)", 0.75f, 0.78f, 0.82f, 1.f);
    y += 22;
    const char* msaa_labels[] = {"OFF", "2x", "4x", "8x", "16x"};
    anchors.msaa_x = ox;
    anchors.msaa_y = y;
    if (draw_button(r, mx, my, ox, y, 120, 32, msaa_labels[msaa_idx])) {
      if (clicked) msaa_open = !msaa_open;
    }
    r.ui_text(ox + 130, y + 8, 1.1f, "(restart to apply)", 0.55f, 0.58f, 0.62f, 1.f);
    y += 50;
    draw_slider(r, mx, my, clicked, ox, y, 280, "FIELD OF VIEW", settings.fov, 60.f, 100.f);
  } else if (tab == OptTab::Graphics) {
    r.ui_text(ox, oy, 1.5f, "GRAPHICS SETTINGS", UiTheme::kOrangeR, UiTheme::kOrangeG,
              UiTheme::kOrangeB, 1.f);
    const float view_top = oy + 34.f;
    const float view_bot = H - 150.f;
    const float view_h = view_bot - view_top;
    // Approximate full content height (must stay in sync with layout below).
    constexpr float kGraphicsContentH = 820.f;
    graphics_scroll = clamp_scroll(graphics_scroll, kGraphicsContentH, view_h);
    r.ui_text(ox + 320, oy + 4, 1.05f, "mouse wheel to scroll", 0.45f, 0.48f, 0.52f, 1.f);

    float mx_d = 0.f, my_d = 0.f;
    ui_mouse_design(r, mx, my, mx_d, my_d);
    const bool in_view = my_d >= view_top && my_d <= view_bot;
    const bool gclick = clicked && in_view;

    auto visible = [&](float y, float h) {
      return y + h > view_top && y < view_bot;
    };

    float y = view_top - graphics_scroll;
    auto row_slider = [&](const char* label, float& value, float vmin, float vmax,
                          const char* hint = nullptr) {
      if (visible(y, 48.f)) {
        draw_slider(r, mx, my, gclick, ox, y, 280, label, value, vmin, vmax);
        if (hint) r.ui_text(ox + 300, y + 24, 1.05f, hint, 0.55f, 0.58f, 0.62f, 1.f);
      }
      y += 52;
    };
    auto row_check = [&](const char* label, bool& flag) {
      if (visible(y, 28.f)) {
        if (draw_checkbox(r, mx, my, ox, y, label, flag) && gclick) flag = !flag;
      }
      y += 32;
    };

    if (visible(y, 40.f)) {
      if (draw_button(r, mx, my, ox, y, 280, 36, "LAPTOP / COMPAT MODE") && gclick) {
        settings.apply_laptop_compat();
      }
      r.ui_text(ox + 295, y + 10, 1.05f, "(fixes green/purple on weak GPUs)", 0.55f, 0.58f, 0.62f,
                1.f);
    }
    y += 48;

    row_slider("DRAW DISTANCE", settings.draw_distance, 1000.f, 12000.f);
    row_slider("FOG SCALE", settings.fog_scale, 0.35f, 2.5f);
    row_slider("RENDER SCALE", settings.render_scale, 0.5f, 1.f, "(internal 3D res)");
    {
      const char* up_labels[] = {"BILINEAR", "FSR 1.0", "AUTO"};
      settings.upscale_mode = std::clamp(settings.upscale_mode, 0, 2);
      if (visible(y, 54.f)) {
        r.ui_text(ox, y, 1.3f, "UPSCALING", 0.75f, 0.78f, 0.82f, 1.f);
        if (draw_button(r, mx, my, ox, y + 22, 160, 32, up_labels[settings.upscale_mode]) &&
            gclick) {
          settings.upscale_mode = (settings.upscale_mode + 1) % 3;
        }
        r.ui_text(ox + 175, y + 30, 1.05f, "(any GPU; Auto picks best)", 0.55f, 0.58f, 0.62f, 1.f);
      }
      y += 62;
    }
    row_slider("FSR SHARPNESS", settings.fsr_sharpness, 0.f, 2.f, "(0=sharp, 2=soft)");
    row_slider("MIP LOD BIAS", settings.mip_lod_bias, -1.f, 2.f, "(+ = cheaper HD mips)");
    row_check("GRASS", settings.grass_enabled);
    row_slider("GRASS DISTANCE", settings.grass_distance, 10.f, 160.f);
    row_check("BLOOM", settings.bloom);
    row_slider("BLOOM INTENSITY", settings.bloom_intensity, 0.f, 1.5f);
    row_check("HDR TONEMAP (float GPU)", settings.hdr);
    row_slider("HDR EXPOSURE", settings.hdr_exposure, 0.15f, 1.5f);
    row_check("SSAO", settings.ssao);
    row_check("SHADOWS", settings.shadows_enabled);
    row_slider("SHADOW DISTANCE", settings.shadow_distance, 400.f, 2800.f);
    {
      const char* sh_labels[] = {"1024", "2048", "4096", "8192"};
      if (visible(y, 54.f)) {
        r.ui_text(ox, y, 1.3f, "SHADOW RESOLUTION", 0.75f, 0.78f, 0.82f, 1.f);
        anchors.shadow_x = ox;
        anchors.shadow_y = y + 22;
        if (draw_button(r, mx, my, ox, y + 22, 120, 32, sh_labels[shadow_idx])) {
          if (gclick) shadow_open = !shadow_open;
        }
        r.ui_text(ox + 130, y + 30, 1.05f, "(restart to apply)", 0.55f, 0.58f, 0.62f, 1.f);
      } else {
        anchors.shadow_x = ox;
        anchors.shadow_y = y + 22;
      }
      y += 62;
    }
    {
      float aniso = static_cast<float>(settings.anisotropic);
      if (visible(y, 48.f)) {
        draw_slider(r, mx, my, gclick, ox, y, 280, "ANISOTROPIC FILTER", aniso, 1.f, 16.f);
        settings.anisotropic = static_cast<int>(aniso + 0.5f);
        r.ui_text(ox + 300, y + 24, 1.05f, "(cap 4–8 for HD packs)", 0.55f, 0.58f, 0.62f, 1.f);
      }
      y += 52;
    }
    // Keep measured content height honest if layout drifts.
    const float measured = (y + graphics_scroll) - view_top;
    if (measured > 1.f)
      graphics_scroll = clamp_scroll(graphics_scroll, measured, view_h);
  } else if (tab == OptTab::Controls) {
    draw_controls_options(r, settings, mx, my, clicked, controls_scroll, rebind_action,
                          capture_key);
  } else {
    r.ui_text(ox, oy, 1.5f, "AUDIO SETTINGS", UiTheme::kOrangeR, UiTheme::kOrangeG,
              UiTheme::kOrangeB, 1.f);
    r.ui_text(ox, oy + 30, 1.2f,
              "Master affects menu music; Effects = weapons/explosions; Music = map score; Voice = "
              "radio cues.",
              0.55f, 0.58f, 0.62f, 1.f);
    float y = oy + 60;
    draw_slider(r, mx, my, clicked, ox, y, 280, "MASTER VOLUME", settings.master_volume, 0.f, 1.f);
    y += 52;
    draw_slider(r, mx, my, clicked, ox, y, 280, "EFFECTS VOLUME", settings.sfx_volume, 0.f, 1.f);
    y += 52;
    draw_slider(r, mx, my, clicked, ox, y, 280, "MUSIC VOLUME", settings.music_volume, 0.f, 1.f);
    y += 52;
    draw_slider(r, mx, my, clicked, ox, y, 280, "VOICE VOLUME", settings.voice_volume, 0.f, 1.f);
  }
}

void draw_options_popups(bf2::Renderer& r, Settings& settings, OptTab tab, int mx, int my,
                           bool clicked, int& res_idx, bool& res_open, int& fs_idx, bool& fs_open,
                           int& msaa_idx, bool& msaa_open, int& shadow_idx, bool& shadow_open,
                           const std::vector<DisplayModeEntry>& modes,
                           const OptionPopupAnchors& anchors, float& res_list_scroll) {
  constexpr float H = 900.f;
  if (tab != OptTab::Video && tab != OptTab::Graphics) return;
  const char* fs_labels[] = {"WINDOWED", "BORDERLESS", "EXCLUSIVE"};
  const char* msaa_labels[] = {"OFF", "2x", "4x", "8x", "16x"};
  const int msaa_vals[] = {0, 2, 4, 8, 16};
  const char* sh_labels[] = {"1024", "2048", "4096", "8192"};
  const int sh_vals[] = {1024, 2048, 4096, 8192};

  if (tab == OptTab::Video && res_open && fs_idx != static_cast<int>(FullscreenMode::Borderless)) {
    const float y0 = anchors.res_y + 36;
    const int n = static_cast<int>(modes.size());
    const float row_h = 30.f;
    const float content_h = static_cast<float>(n) * row_h + 8.f;
    const float max_h = std::max(120.f, H - y0 - 24.f);
    const float list_h = std::min(content_h, max_h);
    res_list_scroll = clamp_scroll(res_list_scroll, content_h, list_h);
    r.ui_rect(anchors.res_x, y0 - 4, 300, list_h, 0.04f, 0.05f, 0.06f, 0.98f);
    const int start = static_cast<int>(res_list_scroll / row_h);
    for (int i = start; i < n && i < start + static_cast<int>(list_h / row_h) + 2; ++i) {
      const float dy = y0 + i * row_h - res_list_scroll;
      if (dy < y0 - 2 || dy > y0 + list_h - row_h) continue;
      if (draw_button(r, mx, my, anchors.res_x + 4, dy, 292, 28, modes[i].label.c_str())) {
        if (clicked) {
          res_idx = i;
          settings.width = modes[i].w;
          settings.height = modes[i].h;
          res_open = false;
        }
      }
    }
  }
  if (tab == OptTab::Video && fs_open) {
    const float y0 = anchors.fs_y + 36;
    r.ui_rect(anchors.fs_x, y0 - 4, 300, 98, 0.04f, 0.05f, 0.06f, 0.98f);
    for (int i = 0; i < 3; ++i) {
      const float dy = y0 + i * 30;
      if (draw_button(r, mx, my, anchors.fs_x + 4, dy, 292, 28, fs_labels[i])) {
        if (clicked) {
          fs_idx = i;
          settings.fullscreen = static_cast<FullscreenMode>(i);
          fs_open = false;
          if (settings.fullscreen == FullscreenMode::Borderless) res_open = false;
        }
      }
    }
  }
  if (tab == OptTab::Video && msaa_open) {
    const float y0 = anchors.msaa_y + 36;
    r.ui_rect(anchors.msaa_x, y0 - 4, 160, 158, 0.04f, 0.05f, 0.06f, 0.98f);
    for (int i = 0; i < 5; ++i) {
      const float dy = y0 + i * 30;
      if (draw_button(r, mx, my, anchors.msaa_x + 4, dy, 152, 28, msaa_labels[i])) {
        if (clicked) {
          msaa_idx = i;
          settings.msaa = msaa_vals[i];
          msaa_open = false;
        }
      }
    }
  }
  if (tab == OptTab::Graphics && shadow_open) {
    const float y0 = anchors.shadow_y + 36;
    r.ui_rect(anchors.shadow_x, y0 - 4, 160, 128, 0.04f, 0.05f, 0.06f, 0.98f);
    for (int i = 0; i < 4; ++i) {
      const float dy = y0 + i * 30;
      if (draw_button(r, mx, my, anchors.shadow_x + 4, dy, 152, 28, sh_labels[i])) {
        if (clicked) {
          shadow_idx = i;
          settings.shadow_res = sh_vals[i];
          shadow_open = false;
        }
      }
    }
  }
}

}  // namespace

std::string resolve_bf2_root(Settings& settings, const char* argv1) {
  if (!settings.bf2_root.empty() && std::filesystem::is_directory(settings.bf2_root))
    return settings.bf2_root;
  if (argv1) {
    std::string p = argv1;
    for (char& c : p)
      if (c == '\\') c = '/';
    const auto pos = p.find("/mods/");
    if (pos != std::string::npos) {
      settings.bf2_root = p.substr(0, pos);
      settings.save();
      return settings.bf2_root;
    }
  }
  const char* probes[] = {
      "C:/Program Files (x86)/Battlefield2",
      "C:/Program Files/Battlefield2",
      "D:/Games/Battlefield 2",
  };
  for (const char* probe : probes) {
    const auto mods = std::filesystem::path(probe) / "mods";
    if (std::filesystem::is_directory(mods)) {
      settings.bf2_root = probe;
      settings.save();
      return settings.bf2_root;
    }
  }
  return {};
}

std::vector<MapEntry> scan_maps(const std::string& bf2_root) {
  std::vector<MapEntry> out;
  if (bf2_root.empty()) return out;
  const auto mods_dir = std::filesystem::path(bf2_root) / "mods";
  if (!std::filesystem::is_directory(mods_dir)) return out;
  for (const auto& mod_entry : std::filesystem::directory_iterator(mods_dir)) {
    if (!mod_entry.is_directory()) continue;
    const auto levels = mod_entry.path() / "Levels";
    if (!std::filesystem::is_directory(levels)) continue;
    const std::string mod_name = mod_entry.path().filename().string();
    for (const auto& map_entry : std::filesystem::directory_iterator(levels)) {
      if (!map_entry.is_directory()) continue;
      const auto server_zip = map_entry.path() / "server.zip";
      if (!std::filesystem::exists(server_zip)) continue;
      MapEntry m;
      m.mod_name = mod_name;
      m.folder = map_entry.path().filename().string();
      m.server_zip = server_zip.string();
      m.display_name = prettify_name(m.folder);
      out.push_back(std::move(m));
    }
  }
  std::sort(out.begin(), out.end(),
            [](const MapEntry& a, const MapEntry& b) { return a.display_name < b.display_name; });
  return out;
}

static std::string normalize_path_key(std::string p) {
  for (char& c : p) {
    if (c == '\\') c = '/';
    else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return p;
}

MapEntry resolve_map_entry(const std::vector<MapEntry>& maps, const std::string& server_zip,
                           const std::string& display_name) {
  if (!server_zip.empty()) {
    const std::string key = normalize_path_key(server_zip);
    for (const auto& m : maps) {
      if (normalize_path_key(m.server_zip) == key) return m;
    }
    if (std::filesystem::exists(server_zip)) {
      MapEntry m;
      m.server_zip = server_zip;
      m.display_name = display_name.empty() ? prettify_name(std::filesystem::path(server_zip)
                                                               .parent_path()
                                                               .filename()
                                                               .string())
                                            : display_name;
      const auto levels = std::filesystem::path(server_zip).parent_path().parent_path();
      if (levels.has_parent_path()) m.mod_name = levels.parent_path().filename().string();
      m.folder = std::filesystem::path(server_zip).parent_path().filename().string();
      return m;
    }
  }
  if (!display_name.empty()) {
    for (const auto& m : maps) {
      if (m.display_name == display_name) return m;
    }
    const std::string want = normalize_path_key(display_name);
    for (const auto& m : maps) {
      if (normalize_path_key(m.display_name) == want) return m;
      if (normalize_path_key(m.folder) == want) return m;
      if (normalize_path_key(prettify_name(m.folder)) == want) return m;
    }
  }
  return {};
}

void apply_graphics_settings(bf2::Renderer& renderer, Settings& settings) {
  renderer.set_bloom(settings.bloom, settings.bloom_intensity);
  renderer.set_hdr(settings.hdr);
  renderer.set_hdr_exposure(settings.hdr_exposure);
  renderer.set_ssao(settings.ssao);
  renderer.set_shadows_enabled(settings.shadows_enabled);
  renderer.set_anisotropic(settings.anisotropic);
  renderer.set_mip_lod_bias(settings.mip_lod_bias);
  renderer.set_upscale_mode(settings.upscale_mode);
  renderer.set_fsr_sharpness(settings.fsr_sharpness);
}

bool run_options_panel(SDL_Window* window, bf2::Renderer& renderer, Settings& settings, int& screen_w,
                       int& screen_h, bool modal) {
  // Free/show the cursor so the menu is clickable (the game captures it).
  SDL_SetRelativeMouseMode(SDL_FALSE);
  SDL_ShowCursor(SDL_ENABLE);
  constexpr float W = 1600.f, H = 900.f;
  std::vector<DisplayModeEntry> display_modes = query_display_modes(window);
  OptTab opt_tab = OptTab::Video;
  int res_idx = find_resolution_index(display_modes, settings.width, settings.height);
  int fs_idx = static_cast<int>(settings.fullscreen);
  int msaa_idx = 0;
  {
    const int vals[] = {0, 2, 4, 8, 16};
    for (int i = 0; i < 5; ++i)
      if (settings.msaa == vals[i]) msaa_idx = i;
  }
  int shadow_idx = 2;
  {
    const int vals[] = {1024, 2048, 4096, 8192};
    for (int i = 0; i < 4; ++i)
      if (settings.shadow_res == vals[i]) shadow_idx = i;
  }
  bool res_open = false, fs_open = false, msaa_open = false, shadow_open = false;
  OptionPopupAnchors anchors{};
  float controls_scroll = 0.f;
  float graphics_scroll = 0.f;
  float res_list_scroll = 0.f;
  int rebind_action = -1;
  bool capture_key = false;
  bool running = true;
  bool done = false;
  bool result = false;

  while (running && !done) {
    SDL_Event e;
    bool clicked = false;
    int mx = 0, my = 0;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        running = false;
        done = true;
        result = false;
      } else if (handle_display_hotkey(window, settings, e)) {
        refresh_display(window, renderer, screen_w, screen_h);
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !capture_key) {
        done = true;
      } else if (opt_tab == OptTab::Controls &&
                 handle_controls_key_capture(e, settings, rebind_action, capture_key)) {
      } else if (e.type == SDL_MOUSEWHEEL && opt_tab == OptTab::Controls) {
        controls_scroll -= e.wheel.y * 24.f;
      } else if (e.type == SDL_MOUSEWHEEL && opt_tab == OptTab::Graphics) {
        graphics_scroll -= e.wheel.y * 36.f;
      } else if (e.type == SDL_MOUSEWHEEL && opt_tab == OptTab::Video && res_open) {
        res_list_scroll -= e.wheel.y * 28.f;
      } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        clicked = true;
      } else if (e.type == SDL_WINDOWEVENT &&
                 (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                  e.window.event == SDL_WINDOWEVENT_RESIZED ||
                  e.window.event == SDL_WINDOWEVENT_RESTORED ||
                  e.window.event == SDL_WINDOWEVENT_MAXIMIZED)) {
        display_modes = query_display_modes(window);
        refresh_display(window, renderer, screen_w, screen_h);
      }
    }
    SDL_GetMouseState(&mx, &my);
    refresh_display(window, renderer, screen_w, screen_h);
    renderer.begin_frame(UiTheme::kBgR, UiTheme::kBgG, UiTheme::kBgB);
    renderer.begin_ui(window);
    if (modal) renderer.ui_rect(0, 0, W, H, 0.f, 0.f, 0.f, 0.65f);
    renderer.ui_rect(40, 40, W - 80, H - 80, 0.06f, 0.07f, 0.08f, 0.98f);
    renderer.ui_rect(40, 40, W - 80, 3, UiTheme::kOrangeR, UiTheme::kOrangeG, UiTheme::kOrangeB, 1.f);
    renderer.ui_text(60, 54, 2.2f, "OPTIONS", 0.95f, 0.96f, 0.98f, 1.f);
    const char* subtabs[] = {"VIDEO", "GRAPHICS", "CONTROLS", "AUDIO"};
    float stx = 60;
    for (int i = 0; i < 4; ++i) {
      const float tw = renderer.ui_text_width(subtabs[i], 1.3f) + 28;
      const bool sel = static_cast<int>(opt_tab) == i;
      const bool hov = rect_hit(renderer, mx, my, stx, 100, tw, 32);
      draw_tab(renderer, stx, 100, tw, 32, subtabs[i], sel, hov);
      if (clicked && hov) opt_tab = static_cast<OptTab>(i);
      stx += tw + 8;
    }
    draw_options_content(renderer, settings, opt_tab, mx, my, clicked, res_idx, res_open, fs_idx,
                         fs_open, msaa_idx, msaa_open, shadow_idx, shadow_open, display_modes,
                         anchors, controls_scroll, graphics_scroll, rebind_action, capture_key);
    // Cover scrolled graphics rows so they don't bleed into APPLY / CANCEL.
    renderer.ui_rect(48, H - 130, W - 96, 90, 0.06f, 0.07f, 0.08f, 1.f);
    const float bx = W - 220, by = H - 100;
    if (draw_button(renderer, mx, my, bx, by, 160, 40, "APPLY", true) && clicked) {
      settings.save();
      apply_window_settings(window, settings);
      apply_graphics_settings(renderer, settings);
      refresh_display(window, renderer, screen_w, screen_h);
      done = true;
      result = true;
    }
    if (draw_button(renderer, mx, my, bx - 180, by, 160, 40, "CANCEL") && clicked) done = true;
    draw_options_popups(renderer, settings, opt_tab, mx, my, clicked, res_idx, res_open, fs_idx,
                        fs_open, msaa_idx, msaa_open, shadow_idx, shadow_open, display_modes,
                        anchors, res_list_scroll);
    renderer.end_ui();
    renderer.end_frame();
    SDL_GL_SwapWindow(window);
  }
  return result;
}

MenuResult run_main_menu(SDL_Window* window, bf2::Renderer& renderer, Settings& settings,
                         const std::vector<MapEntry>& maps) {
  // Free/show the cursor (a prior game session may have captured it).
  SDL_SetRelativeMouseMode(SDL_FALSE);
  SDL_ShowCursor(SDL_ENABLE);
  MenuResult result;
  TopTab tab = TopTab::Play;
  int selected_map = maps.empty() ? -1 : 0;
  float map_scroll = 0.f;
  bool running = true;

  std::vector<DisplayModeEntry> display_modes = query_display_modes(window);
  int res_idx = find_resolution_index(display_modes, settings.width, settings.height);
  int fs_idx = static_cast<int>(settings.fullscreen);
  int msaa_idx = 0;
  {
    const int vals[] = {0, 2, 4, 8, 16};
    for (int i = 0; i < 5; ++i)
      if (settings.msaa == vals[i]) msaa_idx = i;
  }
  int shadow_idx = 2;
  {
    const int vals[] = {1024, 2048, 4096, 8192};
    for (int i = 0; i < 4; ++i)
      if (settings.shadow_res == vals[i]) shadow_idx = i;
  }
  bool res_open = false, fs_open = false, msaa_open = false, shadow_open = false;
  OptTab opt_tab = OptTab::Video;
  OptionPopupAnchors anchors{};
  float controls_scroll = 0.f;
  float graphics_scroll = 0.f;
  float res_list_scroll = 0.f;
  int rebind_action = -1;
  bool capture_key = false;

  MenuMusic bgm;
  if (settings.menu_music_enabled && bgm.init()) {
    const std::string track = resolve_menu_music_path(settings);
    if (!track.empty()) bgm.play_file(track, settings.master_volume);
    else
      std::cerr << "MenuMusic: The_Siege_of_Dalian.mp3 not found (check Downloads or music/ folder)\n";
  }

  MenuBackground bg;
  bg.load(renderer);

  while (running) {
    SDL_Event e;
    bool clicked = false;
    int mx = 0, my = 0;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        result.action = MenuAction::Quit;
        running = false;
      } else if (handle_display_hotkey(window, settings, e)) {
        int sw = 0, sh = 0;
        refresh_display(window, renderer, sw, sh);
      } else if (tab == TopTab::Options && opt_tab == OptTab::Controls &&
                 handle_controls_key_capture(e, settings, rebind_action, capture_key)) {
      } else if (e.type == SDL_MOUSEWHEEL && tab == TopTab::Options && opt_tab == OptTab::Controls) {
        controls_scroll -= e.wheel.y * 24.f;
      } else if (e.type == SDL_MOUSEWHEEL && tab == TopTab::Options && opt_tab == OptTab::Graphics) {
        graphics_scroll -= e.wheel.y * 36.f;
      } else if (e.type == SDL_MOUSEWHEEL && tab == TopTab::Options && opt_tab == OptTab::Video &&
                 res_open) {
        res_list_scroll -= e.wheel.y * 28.f;
      } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        clicked = true;
      } else if (e.type == SDL_MOUSEWHEEL && tab == TopTab::Play) {
        map_scroll -= e.wheel.y * 28.f;
        map_scroll = std::max(0.f, map_scroll);
      } else if (e.type == SDL_WINDOWEVENT &&
                 (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                  e.window.event == SDL_WINDOWEVENT_RESIZED ||
                  e.window.event == SDL_WINDOWEVENT_RESTORED ||
                  e.window.event == SDL_WINDOWEVENT_MAXIMIZED)) {
        display_modes = query_display_modes(window);
        int sw = 0, sh = 0;
        refresh_display(window, renderer, sw, sh);
      }
    }
    SDL_GetMouseState(&mx, &my);

    int sw = 0, sh = 0;
    refresh_display(window, renderer, sw, sh);
    constexpr float W = 1600.f, H = 900.f;

    renderer.begin_frame(UiTheme::kBgR, UiTheme::kBgG, UiTheme::kBgB);
    renderer.begin_ui(window);

    bg.draw(renderer);

    renderer.ui_rect(0, 0, W, 52, 0.03f, 0.04f, 0.05f, 0.85f);
    renderer.ui_rect(0, 52, W, 2, UiTheme::kOrangeR, UiTheme::kOrangeG, UiTheme::kOrangeB, 1.f);
    renderer.ui_text(24, 14, 2.4f, "PROJECT DALIAN", 0.95f, 0.96f, 0.98f, 1.f);

    const char* top_labels[] = {"PLAY", "OPTIONS", "MULTIPLAYER", "COMMUNITY", "QUIT"};
    float tx = 280;
    for (int i = 0; i < 5; ++i) {
      const float tw = renderer.ui_text_width(top_labels[i], 1.4f) + 24;
      const bool active = (i == 0 || i == 1 || i == 2 || i == 4);
      const bool sel = (i == 0 && tab == TopTab::Play) || (i == 1 && tab == TopTab::Options) ||
                       (i == 2 && tab == TopTab::Multiplayer) || (i == 4 && tab == TopTab::Quit);
      const bool hov = active && rect_hit(renderer, mx, my, tx, 8, tw, 36);
      if (active) {
        draw_tab(renderer, tx, 8, tw, 36, top_labels[i], sel, hov);
        if (clicked && hov) {
          if (i == 0) tab = TopTab::Play;
          else if (i == 1) tab = TopTab::Options;
          else if (i == 2) tab = TopTab::Multiplayer;
          else if (i == 4) {
            result.action = MenuAction::Quit;
            running = false;
          }
        }
      } else {
        renderer.ui_text(tx + 12, 18, 1.4f, top_labels[i], 0.35f, 0.36f, 0.38f, 1.f);
      }
      tx += tw + 6;
    }

    renderer.ui_rect(20, 64, W - 40, H - 84, 0.06f, 0.07f, 0.08f, 0.82f);

    if (tab == TopTab::Play) {
      const float lx = 40, ly = 80, lw = 420;
      renderer.ui_text(lx, ly, 1.6f, "SELECT MAP", UiTheme::kOrangeR, UiTheme::kOrangeG,
                       UiTheme::kOrangeB, 1.f);
      const float list_y = ly + 32;
      const float list_h = H - list_y - 120;
      draw_map_list_panel(renderer, mx, my, clicked, maps, lx, list_y, lw, list_h, selected_map,
                          map_scroll);
      const float dx = lx + lw + 30;
      if (selected_map >= 0 && selected_map < static_cast<int>(maps.size())) {
        const MapEntry& m = maps[selected_map];
        draw_clipped_text(renderer, dx, ly, W - dx - 260.f, 2.0f, m.display_name.c_str(), 0.95f,
                          0.96f, 0.98f, 1.f);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Mod: [%s]", m.mod_name.c_str());
        draw_clipped_text(renderer, dx, ly + 36, W - dx - 260.f, 1.4f, buf, 0.65f, 0.68f, 0.72f,
                          1.f);
        std::snprintf(buf, sizeof(buf), "Folder: %s", m.folder.c_str());
        draw_clipped_text(renderer, dx, ly + 58, W - dx - 260.f, 1.2f, buf, 0.55f, 0.58f, 0.62f,
                          1.f);
      } else {
        renderer.ui_text(dx, ly, 1.6f, "No maps found.", 0.7f, 0.72f, 0.75f, 1.f);
        renderer.ui_text(dx, ly + 28, 1.2f, "Set BF2 install path in Options or launch with a map path.",
                          0.55f, 0.58f, 0.62f, 1.f);
      }
      const float bx = W - 220, by = H - 100;
      const bool can_start = selected_map >= 0 && selected_map < static_cast<int>(maps.size());
      if (can_start && draw_button(renderer, mx, my, bx, by, 180, 48, "START", true) && clicked) {
        result.action = MenuAction::StartMap;
        result.map = maps[selected_map];
        running = false;
      }
    } else if (tab == TopTab::Multiplayer) {
      renderer.ui_text(40, 80, 2.0f, "MULTIPLAYER", 0.95f, 0.96f, 0.98f, 1.f);
      draw_clipped_text(renderer, 40, 118, W - 80.f, 1.35f,
                        "Play cooperatively over LAN or Tailscale. Host a lobby or join an existing "
                        "match.",
                        0.6f, 0.63f, 0.68f, 1.f);
      draw_clipped_text(renderer, 40, 150, W - 80.f, 1.2f,
                        "Choose your faction and army in the lobby or deploy screen.", 0.55f, 0.58f,
                        0.62f, 1.f);
      if (draw_button(renderer, mx, my, 40, H - 120, 260, 52, "ENTER MULTIPLAYER", true) &&
          clicked) {
        if (run_multiplayer_flow(window, renderer, settings, maps, result)) running = false;
      }
    } else if (tab == TopTab::Options) {
      const char* subtabs[] = {"VIDEO", "GRAPHICS", "CONTROLS", "AUDIO"};
      float stx = 48;
      for (int i = 0; i < 4; ++i) {
        const float tw = renderer.ui_text_width(subtabs[i], 1.3f) + 28;
        const bool sel = static_cast<int>(opt_tab) == i;
        const bool hov = rect_hit(renderer, mx, my, stx, 80, tw, 32);
        draw_tab(renderer, stx, 80, tw, 32, subtabs[i], sel, hov);
        if (clicked && hov) opt_tab = static_cast<OptTab>(i);
        stx += tw + 8;
      }
      draw_options_content(renderer, settings, opt_tab, mx, my, clicked, res_idx, res_open, fs_idx,
                           fs_open, msaa_idx, msaa_open, shadow_idx, shadow_open, display_modes,
                           anchors, controls_scroll, graphics_scroll, rebind_action, capture_key);
      // Mask bottom chrome so scrolled settings don't overlay path / APPLY / footer.
      renderer.ui_rect(40, H - 145, W - 80, 117, 0.06f, 0.07f, 0.08f, 1.f);
      float y = H - 130;
      renderer.ui_text(48, y, 1.2f, "BF2 INSTALL PATH:", 0.75f, 0.78f, 0.82f, 1.f);
      draw_clipped_text(renderer, 48, y + 22, W - 320.f, 1.1f,
                        settings.bf2_root.empty() ? "(not set)" : settings.bf2_root.c_str(), 0.55f,
                        0.58f, 0.62f, 1.f);
      const float bx = W - 220, by = H - 100;
      if (draw_button(renderer, mx, my, bx, by, 160, 40, "APPLY", true) && clicked) {
        settings.save();
        apply_window_settings(window, settings);
        apply_graphics_settings(renderer, settings);
        bgm.set_volume(settings.master_volume);
        int sw = 0, sh = 0;
        refresh_display(window, renderer, sw, sh);
      }
      draw_options_popups(renderer, settings, opt_tab, mx, my, clicked, res_idx, res_open, fs_idx,
                          fs_open, msaa_idx, msaa_open, shadow_idx, shadow_open, display_modes,
                          anchors, res_list_scroll);
    }

    draw_clipped_text(renderer, 24, H - 28, W - 48.f, 1.1f,
                      "Alt+Enter / F11 display  |  Ctrl+Shift+W safe windowed  |  Alt+F4 quit",
                      0.45f, 0.48f, 0.52f, 1.f);
    renderer.end_ui();
    renderer.end_frame();
    SDL_GL_SwapWindow(window);
  }
  bg.destroy(renderer);
  bgm.shutdown();
  return result;
}

PauseResult run_pause_overlay(SDL_Window* window, bf2::Renderer& renderer, Settings& settings,
                              int& screen_w, int& screen_h, bool* options_open) {
  // Release the captured mouse so the cursor shows and the buttons are clickable.
  SDL_SetRelativeMouseMode(SDL_FALSE);
  SDL_ShowCursor(SDL_ENABLE);
  PauseResult pr;
  bool running = true;
  bool show_options = options_open && *options_open;
  constexpr float W = 1600.f, H = 900.f;

  while (running) {
    if (show_options) {
      run_options_panel(window, renderer, settings, screen_w, screen_h, true);
      refresh_display(window, renderer, screen_w, screen_h);
      if (options_open) *options_open = false;
      show_options = false;
      continue;
    }
    SDL_Event e;
    bool clicked = false;
    int mx = 0, my = 0;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        pr.quit_app = true;
        running = false;
      } else if (handle_display_hotkey(window, settings, e)) {
        refresh_display(window, renderer, screen_w, screen_h);
      } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        pr.resume = true;
        running = false;
      } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        clicked = true;
      } else if (e.type == SDL_WINDOWEVENT &&
                 (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                  e.window.event == SDL_WINDOWEVENT_RESIZED)) {
        refresh_display(window, renderer, screen_w, screen_h);
      }
    }
    SDL_GetMouseState(&mx, &my);
    refresh_display(window, renderer, screen_w, screen_h);
    renderer.begin_frame(UiTheme::kBgR * 0.3f, UiTheme::kBgG * 0.3f, UiTheme::kBgB * 0.3f);
    renderer.begin_ui(window);
    renderer.ui_rect(0, 0, W, H, 0.f, 0.f, 0.f, 0.55f);
    const float pw = 320, ph = 320;
    const float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
    renderer.ui_rect(px, py, pw, ph, 0.06f, 0.07f, 0.08f, 0.98f);
    renderer.ui_rect(px, py, pw, 3, UiTheme::kOrangeR, UiTheme::kOrangeG, UiTheme::kOrangeB, 1.f);
    renderer.ui_text(px + 20, py + 16, 2.0f, "PAUSED", 0.95f, 0.96f, 0.98f, 1.f);
    const float bx = px + 40, bw = pw - 80, bh = 44;
    float by = py + 70;
    if (draw_button(renderer, mx, my, bx, by, bw, bh, "RESUME", true) && clicked) {
      pr.resume = true;
      running = false;
    }
    by += 56;
    if (draw_button(renderer, mx, my, bx, by, bw, bh, "OPTIONS") && clicked) show_options = true;
    by += 56;
    if (draw_button(renderer, mx, my, bx, by, bw, bh, "LEAVE TO MENU") && clicked) {
      pr.leave_to_menu = true;
      running = false;
    }
    by += 56;
    if (draw_button(renderer, mx, my, bx, by, bw, bh, "QUIT") && clicked) {
      pr.quit_app = true;
      running = false;
    }
    renderer.end_ui();
    renderer.end_frame();
    SDL_GL_SwapWindow(window);
  }
  return pr;
}

void draw_map_list_panel(bf2::Renderer& renderer, int mx, int my, bool clicked,
                         const std::vector<MapEntry>& maps, float lx, float list_y, float lw,
                         float list_h, int& selected_map, float& map_scroll) {
  renderer.ui_rect(lx, list_y, lw, list_h, 0.04f, 0.05f, 0.06f, 0.95f);
  const float header_h = 22.f;
  draw_clipped_text(renderer, lx + 12, list_y + 4, lw - 160.f, 1.0f, "MAP", 0.55f, 0.58f, 0.62f,
                    1.f);
  draw_clipped_text(renderer, lx + lw - 132, list_y + 4, 120.f, 1.0f, "MOD", 0.55f, 0.58f, 0.62f,
                    1.f);
  renderer.ui_rect(lx + 8, list_y + header_h - 2, lw - 16, 1, 0.14f, 0.15f, 0.16f, 1.f);
  const float row_h = 36.f;
  const float body_y = list_y + header_h;
  const float body_h = list_h - header_h;
  const int visible = static_cast<int>(body_h / row_h) + 1;
  const int max_scroll = std::max(0, static_cast<int>(maps.size()) - visible + 1);
  map_scroll = std::min(map_scroll, static_cast<float>(max_scroll) * row_h);
  map_scroll = std::max(0.f, map_scroll);
  const int start = static_cast<int>(map_scroll / row_h);
  for (int i = start; i < static_cast<int>(maps.size()) && i < start + visible + 2; ++i) {
    const float ry = body_y + i * row_h - map_scroll;
    if (ry < body_y || ry > body_y + body_h - row_h) continue;
    const bool sel = i == selected_map;
    const bool hov = rect_hit(renderer, mx, my, lx, ry, lw, row_h);
    renderer.ui_rect(lx, ry, lw, row_h - 2, sel ? 0.18f : (hov ? 0.12f : 0.07f),
                     sel ? 0.35f : (hov ? 0.18f : 0.09f), sel ? 0.55f : (hov ? 0.22f : 0.11f),
                     0.95f);
    if (sel)
      renderer.ui_rect(lx, ry, 4, row_h - 2, UiTheme::kOrangeR, UiTheme::kOrangeG,
                       UiTheme::kOrangeB, 1.f);
    draw_clipped_text(renderer, lx + 12, ry + 10, lw - 160.f, 1.35f, maps[i].display_name.c_str(),
                      0.92f, 0.94f, 0.96f, 1.f);
    char mod_buf[96];
    std::snprintf(mod_buf, sizeof(mod_buf), "[%s]", maps[i].mod_name.c_str());
    draw_clipped_text(renderer, lx + lw - 132, ry + 12, 120.f, 1.1f, mod_buf, UiTheme::kOrangeR,
                      UiTheme::kOrangeG, UiTheme::kOrangeB, 1.f);
    if (clicked && hov) selected_map = i;
  }
}

void run_connection_lost_dialog(SDL_Window* window, bf2::Renderer& renderer, int& screen_w,
                                int& screen_h, const char* title, const char* message) {
  SDL_SetRelativeMouseMode(SDL_FALSE);
  SDL_ShowCursor(SDL_ENABLE);
  constexpr float W = 1600.f, H = 900.f;
  bool running = true;
  while (running) {
    SDL_Event e;
    bool clicked = false;
    int mx = 0, my = 0;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        running = false;
        break;
      } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        clicked = true;
      }
    }
    SDL_GetMouseState(&mx, &my);
    refresh_display(window, renderer, screen_w, screen_h);
    renderer.begin_frame(0.04f, 0.05f, 0.06f);
    renderer.begin_ui(window);
    renderer.ui_rect(0, 0, W, H, 0.f, 0.f, 0.f, 0.65f);
    const float pw = 520.f, ph = 280.f;
    const float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
    renderer.ui_rect(px, py, pw, ph, 0.06f, 0.07f, 0.08f, 0.98f);
    renderer.ui_rect(px, py, pw, 3, UiTheme::kOrangeR, UiTheme::kOrangeG, UiTheme::kOrangeB, 1.f);
    draw_clipped_text(renderer, px + 24, py + 20, pw - 48.f, 2.0f, title, 0.95f, 0.96f, 0.98f, 1.f);
    draw_clipped_text(renderer, px + 24, py + 64, pw - 48.f, 1.45f, message, 0.72f, 0.74f, 0.78f,
                      1.f);
    const float bx = px + (pw - 240.f) * 0.5f, by = py + ph - 72.f;
    if (draw_button(renderer, mx, my, bx, by, 240, 48, "BACK TO MAIN MENU", true) && clicked) {
      running = false;
    }
    renderer.end_ui();
    renderer.end_frame();
    SDL_GL_SwapWindow(window);
  }
}

}  // namespace dalian
