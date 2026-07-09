#include "multiplayer_menu.hpp"

#include "factions.hpp"
#include "server_discovery.hpp"
#include "ui_input.hpp"
#include "ui_layout.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace dalian {
namespace {

bool rect_hit(const bf2::Renderer& r, int mx, int my, float x, float y, float w, float h) {
  return ui_hit(r, mx, my, x, y, w, h);
}

struct UiTheme {
  static constexpr float kOrangeR = 0.95f, kOrangeG = 0.55f, kOrangeB = 0.08f;
};

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
  const bool hov = rect_hit(r, mx, my, x, y, 320, bs + 4);
  r.ui_rect(x, y, bs, bs, 0.08f, 0.09f, 0.10f, 1.f);
  if (checked) {
    r.ui_rect(x + 3, y + 3, bs - 6, bs - 6, UiTheme::kOrangeR, UiTheme::kOrangeG,
              UiTheme::kOrangeB, 1.f);
  }
  r.ui_text(x + bs + 10, y + 2, 1.4f, label, hov ? 1.f : 0.85f, hov ? 0.88f : 0.82f,
            hov ? 0.90f : 0.84f, 1.f);
  return hov;
}

enum class MpView { Browse, HostSetup, Lobby };

void draw_faction_picker(bf2::Renderer& r, int mx, int my, bool clicked, float x, float y,
                         float w, float h, int& faction_id, float& scroll) {
  r.ui_text(x, y - 22, 1.4f, "FACTION / ARMY", 0.75f, 0.78f, 0.82f, 1.f);
  r.ui_rect(x, y, w, h, 0.04f, 0.05f, 0.06f, 0.95f);
  const float row_h = 28.f;
  const int visible = static_cast<int>(h / row_h);
  scroll = std::clamp(scroll, 0.f, std::max(0.f, static_cast<float>(faction_count()) * row_h - h));
  const int start = static_cast<int>(scroll / row_h);
  for (std::size_t i = start; i < faction_count() && static_cast<int>(i) < start + visible + 2;
       ++i) {
    const float ry = y + static_cast<float>(i) * row_h - scroll;
    if (ry < y || ry > y + h - row_h) continue;
    const bool sel = static_cast<int>(i) == faction_id;
    const bool hov = rect_hit(r, mx, my, x, ry, w, row_h);
    r.ui_rect(x, ry, w, row_h - 2, sel ? 0.16f : (hov ? 0.11f : 0.07f),
              sel ? 0.28f : (hov ? 0.14f : 0.09f), sel ? 0.42f : (hov ? 0.18f : 0.11f), 0.96f);
    const auto& f = faction_at(static_cast<int>(i));
    char line[128];
    std::snprintf(line, sizeof(line), "%s — %s", f.country, f.army);
    r.ui_text(x + 8, ry + 6, 1.15f, line, 0.9f, 0.92f, 0.94f, 1.f);
    if (clicked && hov) faction_id = static_cast<int>(i);
  }
}

}  // namespace

bool run_multiplayer_flow(SDL_Window* window, bf2::Renderer& renderer, Settings& settings,
                          const std::vector<MapEntry>& maps, MenuResult& result) {
  SDL_SetRelativeMouseMode(SDL_FALSE);
  SDL_ShowCursor(SDL_ENABLE);

  MpView view = MpView::Browse;
  ServerBrowser browser;
  DiscoveryHost discovery;
  std::unique_ptr<bf2::Net> net;
  bool scan_started = false;
  int selected_server = -1;
  int selected_map = maps.empty() ? -1 : 0;
  float map_scroll = 0.f;
  float faction_scroll = 0.f;
  bool allow_late_join = settings.allow_late_join;
  bool use_tailscale = settings.use_tailscale;
  TextField name_field{};
  TextField ip_field{};
  ip_field.numeric_ip = true;
  if (settings.player_name.empty())
    std::snprintf(name_field.buf, sizeof(name_field.buf), "Player");
  else
    std::strncpy(name_field.buf, settings.player_name.c_str(), sizeof(name_field.buf) - 1);
  std::strncpy(ip_field.buf, settings.manual_server_ip.c_str(), sizeof(ip_field.buf) - 1);
  int faction_id = settings.default_faction;
  bool local_ready = false;
  bool is_host = false;
  MapEntry host_map{};
  float lobby_anim = 0.f;

  const std::string tailscale_subnet =
      settings.tailscale_subnet.empty() ? detect_tailscale_subnet() : settings.tailscale_subnet;

  bool discovery_active = false;
  bool name_was_focused = false;
  bool running = true;
  while (running) {
    if (view == MpView::Browse && !scan_started) {
      browser.begin_scan(settings.net_port, true, use_tailscale, tailscale_subnet);
      scan_started = true;
    }
    browser.poll();

    if (net) net->poll(1.f / 60.f);

    if (view == MpView::Lobby && net && name_was_focused && !name_field.focused &&
        net->lobby_joined()) {
      net->send_join_info(name_field.buf, static_cast<std::uint16_t>(faction_id));
    }
    name_was_focused = name_field.focused;

    if (net && is_host && view == MpView::Lobby) {
      if (!discovery_active) {
        discovery.start(settings.net_port);
        discovery_active = true;
      }
      DiscoveryAdvert adv{};
      adv.game_port = settings.net_port;
      adv.host_name = name_field.buf;
      adv.allow_late_join = allow_late_join;
      adv.players = static_cast<int>(net->lobby().members.size());
      adv.in_game = net->game_started();
      adv.map_name = net->lobby().map_name.empty() ? host_map.display_name : net->lobby().map_name;
      discovery.set_advert(adv);
      discovery.poll();
    } else if (discovery_active) {
      discovery.stop();
      discovery_active = false;
    }

    if (net && view == MpView::Lobby && net->game_started()) {
      result.action = MenuAction::StartMultiplayer;
      if (!host_map.server_zip.empty())
        result.map = host_map;
      else if (selected_map >= 0 && selected_map < static_cast<int>(maps.size()))
        result.map = maps[selected_map];
      result.mp.enabled = true;
      result.mp.is_host = is_host;
      result.mp.port = settings.net_port;
      result.mp.player_name = name_field.buf;
      result.mp.faction_id = faction_id;
      result.mp.allow_late_join = allow_late_join;
      result.net = std::move(net);
      discovery.stop();
      text_field_blur(name_field);
      text_field_blur(ip_field);
      settings.player_name = name_field.buf;
      settings.use_tailscale = use_tailscale;
      settings.allow_late_join = allow_late_join;
      settings.default_faction = faction_id;
      settings.manual_server_ip = ip_field.buf;
      settings.save();
      return true;
    }

    SDL_Event e;
    bool clicked = false;
    int mx = 0, my = 0;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        text_field_blur(name_field);
        text_field_blur(ip_field);
        discovery.stop();
        return false;
      }
      if (handle_display_hotkey(window, settings, e)) {
      } else if (e.type == SDL_KEYDOWN) {
        text_field_handle_keydown(name_field, e.key);
        text_field_handle_keydown(ip_field, e.key);
      } else if (e.type == SDL_TEXTINPUT) {
        text_field_handle_text(name_field, e.text.text);
        text_field_handle_text(ip_field, e.text.text);
      }
      if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) clicked = true;
      if (e.type == SDL_MOUSEWHEEL) {
        if (view == MpView::HostSetup) map_scroll -= e.wheel.y * 28.f;
        faction_scroll -= e.wheel.y * 24.f;
      }
    }
    SDL_GetMouseState(&mx, &my);

    int sw = 0, sh = 0;
    sync_drawable_size(window, sw, sh);
    constexpr float W = 1600.f, H = 900.f;
    lobby_anim += 0.05f;

    renderer.begin_frame(0.04f, 0.05f, 0.06f);
    renderer.begin_ui(window);
    renderer.ui_rect(20, 64, W - 40, H - 84, 0.06f, 0.07f, 0.08f, 0.92f);

    if (view == MpView::Browse) {
      renderer.ui_text(40, 80, 2.0f, "MULTIPLAYER", 0.95f, 0.96f, 0.98f, 1.f);
      renderer.ui_text(40, 112, 1.3f, "Find games on LAN and Tailscale, or host a new lobby.",
                       0.6f, 0.63f, 0.68f, 1.f);

      if (draw_checkbox(renderer, mx, my, 40, 140, "Use Tailscale network scan", use_tailscale) &&
          clicked)
        use_tailscale = !use_tailscale;

      renderer.ui_text(40, 178, 1.2f, "Manual IP (click to type):", 0.7f, 0.72f, 0.76f, 1.f);
      draw_text_field(renderer, mx, my, clicked, 40, 200, 220, 32, ip_field, "e.g. 100.x.x.x",
                      &name_field);

      const float lx = 40, ly = 250, lw = W * 0.55f, lh = H - 360;
      renderer.ui_text(lx, ly - 24, 1.4f, browser.scanning() ? "Searching for games..." : "Servers",
                       UiTheme::kOrangeR, UiTheme::kOrangeG, UiTheme::kOrangeB, 1.f);
      if (browser.scanning()) {
        const int dots = static_cast<int>(lobby_anim) % 4;
        char buf[8] = "   ";
        for (int i = 0; i < dots; ++i) buf[i] = '.';
        renderer.ui_text(lx + 220, ly - 24, 1.4f, buf, 0.7f, 0.72f, 0.76f, 1.f);
      }
      renderer.ui_rect(lx, ly, lw, lh, 0.04f, 0.05f, 0.06f, 0.95f);
      const float row_h = 40.f;
      const auto& servers = browser.servers();
      for (std::size_t i = 0; i < servers.size(); ++i) {
        const float ry = ly + static_cast<float>(i) * row_h;
        if (ry > ly + lh - row_h) break;
        const bool sel = static_cast<int>(i) == selected_server;
        const bool hov = rect_hit(renderer, mx, my, lx, ry, lw, row_h);
        renderer.ui_rect(lx, ry, lw, row_h - 2, sel ? 0.16f : (hov ? 0.11f : 0.07f),
                         sel ? 0.28f : (hov ? 0.14f : 0.09f), sel ? 0.42f : (hov ? 0.18f : 0.11f),
                         0.96f);
        char line[256];
        std::snprintf(line, sizeof(line), "%s:%u  %s  (%d players)%s", servers[i].address.c_str(),
                      servers[i].game_port, servers[i].map_name.c_str(), servers[i].players,
                      servers[i].in_game ? " [IN GAME]" : "");
        renderer.ui_text(lx + 10, ry + 10, 1.25f, line, 0.9f, 0.92f, 0.94f, 1.f);
        if (clicked && hov) selected_server = static_cast<int>(i);
      }

      const float rx = lx + lw + 24;
      renderer.ui_text(rx, ly - 24, 1.4f, "PLAYER NAME (click to type)", 0.75f, 0.78f, 0.82f, 1.f);
      draw_text_field(renderer, mx, my, clicked, rx, ly, 280, 32, name_field, "Player", &ip_field);

      draw_faction_picker(renderer, mx, my, clicked, rx, ly + 50, 280, 220, faction_id,
                          faction_scroll);

      if (draw_button(renderer, mx, my, lx, H - 120, 140, 40, "REFRESH") && clicked) {
        scan_started = false;
        browser.clear();
      }
      if (draw_button(renderer, mx, my, lx + 150, H - 120, 140, 40, "HOST GAME") && clicked)
        view = MpView::HostSetup;
      const bool can_join =
          ((selected_server >= 0 && selected_server < static_cast<int>(servers.size()) &&
            (!servers[selected_server].in_game || servers[selected_server].allow_late_join)) ||
           ip_field.buf[0] != '\0');
      if (can_join && draw_button(renderer, mx, my, W - 260, H - 120, 180, 48, "JOIN", true) &&
          clicked) {
        bool blocked = false;
        std::string addr = ip_field.buf;
        std::uint16_t port = settings.net_port;
        if (selected_server >= 0 && selected_server < static_cast<int>(servers.size())) {
          const auto& sel = servers[selected_server];
          if (sel.in_game && !sel.allow_late_join) {
            blocked = true;
          } else {
            addr = sel.address;
            port = sel.game_port;
            for (const auto& m : maps) {
              if (m.display_name == sel.map_name) {
                host_map = m;
                break;
              }
            }
          }
        }
        if (!blocked && !addr.empty()) {
          net = std::make_unique<bf2::Net>();
          if (net->connect(addr, port)) {
            net->set_lobby_mode(true);
            is_host = false;
            view = MpView::Lobby;
          } else {
            net.reset();
          }
        }
      }
      if (draw_button(renderer, mx, my, 40, H - 120, 100, 40, "BACK") && clicked) {
        text_field_blur(name_field);
        text_field_blur(ip_field);
        discovery.stop();
        return false;
      }
    } else if (view == MpView::HostSetup) {
      renderer.ui_text(40, 80, 2.0f, "HOST NEW GAME", 0.95f, 0.96f, 0.98f, 1.f);
      const float lx = 40, ly = 120, lw = 380;
      renderer.ui_text(lx, ly, 1.4f, "SELECT MAP", UiTheme::kOrangeR, UiTheme::kOrangeG,
                       UiTheme::kOrangeB, 1.f);
      const float list_y = ly + 28;
      const float list_h = 220.f;
      renderer.ui_rect(lx, list_y, lw, list_h, 0.04f, 0.05f, 0.06f, 0.95f);
      const float row_h = 32.f;
      map_scroll = std::max(0.f, map_scroll);
      const int start = static_cast<int>(map_scroll / row_h);
      for (int i = start; i < static_cast<int>(maps.size()) && i < start + 10; ++i) {
        const float ry = list_y + i * row_h - map_scroll;
        const bool sel = i == selected_map;
        const bool hov = rect_hit(renderer, mx, my, lx, ry, lw, row_h);
        renderer.ui_rect(lx, ry, lw, row_h - 2, sel ? 0.16f : (hov ? 0.11f : 0.07f),
                         sel ? 0.28f : (hov ? 0.14f : 0.09f), sel ? 0.42f : (hov ? 0.18f : 0.11f),
                         0.96f);
        renderer.ui_text(lx + 10, ry + 8, 1.2f, maps[i].display_name.c_str(), 0.9f, 0.92f, 0.94f,
                         1.f);
        if (clicked && hov) selected_map = i;
      }

      char port_buf[16];
      std::snprintf(port_buf, sizeof(port_buf), "Port: %u", settings.net_port);
      renderer.ui_text(lx, list_y + list_h + 16, 1.3f, port_buf, 0.7f, 0.72f, 0.76f, 1.f);

      if (draw_checkbox(renderer, mx, my, lx, list_y + list_h + 44, "Allow join after game start",
                        allow_late_join) &&
          clicked)
        allow_late_join = !allow_late_join;

      draw_faction_picker(renderer, mx, my, clicked, lx + 420, ly, 360, H - 280, faction_id,
                          faction_scroll);

      renderer.ui_text(lx, list_y + list_h + 72, 1.2f, "PLAYER NAME (click to type):", 0.7f, 0.72f,
                       0.76f, 1.f);
      draw_text_field(renderer, mx, my, clicked, lx, list_y + list_h + 94, 280, 32, name_field,
                      "Player", &ip_field);

      if (selected_map >= 0 && draw_button(renderer, mx, my, W - 260, H - 120, 200, 48,
                                           "CREATE LOBBY", true) &&
          clicked) {
        host_map = maps[selected_map];
        net = std::make_unique<bf2::Net>();
        if (net->host(settings.net_port, false)) {
          net->set_lobby_mode(true);
          net->set_lobby_config(allow_late_join, host_map.display_name);
          net->send_join_info(name_field.buf, static_cast<std::uint16_t>(faction_id));
          is_host = true;
          view = MpView::Lobby;
        } else {
          net.reset();
        }
      }
      if (draw_button(renderer, mx, my, 40, H - 120, 100, 40, "BACK") && clicked)
        view = MpView::Browse;
    } else if (view == MpView::Lobby && net) {
      renderer.ui_text(40, 80, 2.0f, "LOBBY", 0.95f, 0.96f, 0.98f, 1.f);
      if (!net->connected() && !net->is_server()) {
        renderer.ui_text(40, 120, 1.4f, "Connecting...", 0.7f, 0.72f, 0.76f, 1.f);
      } else if (!net->lobby_joined()) {
        net->send_join_info(name_field.buf, static_cast<std::uint16_t>(faction_id));
      }

      const auto& lob = net->lobby();
      char buf[256];
      std::snprintf(buf, sizeof(buf), "Map: %s", lob.map_name.empty() ? host_map.display_name.c_str()
                                                                      : lob.map_name.c_str());
      renderer.ui_text(40, 118, 1.4f, buf, 0.65f, 0.68f, 0.72f, 1.f);
      std::snprintf(buf, sizeof(buf), "Late join: %s", lob.allow_late_join ? "ON" : "OFF");
      renderer.ui_text(40, 142, 1.2f, buf, 0.55f, 0.58f, 0.62f, 1.f);

      renderer.ui_text(40, 180, 1.5f, "PLAYERS", UiTheme::kOrangeR, UiTheme::kOrangeG,
                       UiTheme::kOrangeB, 1.f);
      renderer.ui_text(W - 400, 156, 1.2f, "PLAYER NAME (click to type):", 0.7f, 0.72f, 0.76f, 1.f);
      draw_text_field(renderer, mx, my, clicked, W - 400, 178, 360, 32, name_field, "Player",
                      nullptr);
      float py = 210;
      for (const auto& m : lob.members) {
        const auto& f = faction_at(static_cast<int>(m.faction_id));
        std::snprintf(buf, sizeof(buf), "%s%s — %s (%s)%s", m.is_host ? "[HOST] " : "", m.name.c_str(),
                      f.country, f.army, m.ready ? "  READY" : "");
        renderer.ui_text(48, py, 1.25f, buf, 0.9f, 0.92f, 0.94f, 1.f);
        py += 26;
      }

      draw_faction_picker(renderer, mx, my, clicked, W - 400, 220, 360, H - 360, faction_id,
                          faction_scroll);
      if (clicked && faction_id >= 0) net->set_faction(static_cast<std::uint16_t>(faction_id));

      if (draw_checkbox(renderer, mx, my, 40, H - 180, "Ready", local_ready) && clicked) {
        local_ready = !local_ready;
        net->set_ready(local_ready);
      }

      if (is_host) {
        if (draw_checkbox(renderer, mx, my, 40, H - 220, "Allow join after game start",
                          allow_late_join) &&
            clicked) {
          allow_late_join = !allow_late_join;
          net->set_lobby_config(allow_late_join, host_map.display_name);
        }
        if (draw_button(renderer, mx, my, W - 260, H - 120, 200, 48, "START GAME", true) &&
            clicked) {
          net->host_start_game();
        }
      } else {
        renderer.ui_text(40, H - 220, 1.3f, "Waiting for host to start...", 0.6f, 0.63f, 0.68f, 1.f);
      }
      if (draw_button(renderer, mx, my, 40, H - 120, 100, 40, "LEAVE") && clicked) {
        text_field_blur(name_field);
        text_field_blur(ip_field);
        net.reset();
        discovery.stop();
        view = MpView::Browse;
        scan_started = false;
      }
    }

    renderer.end_ui();
    renderer.end_frame();
    SDL_GL_SwapWindow(window);
  }
  text_field_blur(name_field);
  text_field_blur(ip_field);
  discovery.stop();
  return false;
}

}  // namespace dalian
