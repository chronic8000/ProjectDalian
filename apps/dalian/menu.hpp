#pragma once

#include <SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app_settings.hpp"
#include "engine/net/net.hpp"
#include "engine/render/renderer.hpp"

namespace dalian {

struct MapEntry {
  std::string mod_name;
  std::string folder;
  std::string server_zip;
  std::string display_name;
};

struct MultiplayerConfig {
  bool enabled = false;
  bool is_host = false;
  bool allow_late_join = true;
  std::uint16_t port = 27015;
  std::string player_name;
  int faction_id = 0;
  bool bots_enabled = true;
  int bot_count = 28;
  int bot_difficulty = 3;  // 1 easy .. 5 hard
};

enum class MenuAction { None, StartMap, StartMultiplayer, Quit, LeaveToMenu };

struct MenuResult {
  MenuAction action = MenuAction::None;
  MapEntry map{};
  MultiplayerConfig mp{};
  std::unique_ptr<bf2::Net> net;
};

struct PauseResult {
  bool resume = false;
  bool open_options = false;
  bool leave_to_menu = false;
  bool quit_app = false;
};

std::string resolve_bf2_root(Settings& settings, const char* argv1);
std::vector<MapEntry> scan_maps(const std::string& bf2_root);
// Resolve a map from lobby/discovery data (server.zip path preferred, then display name).
MapEntry resolve_map_entry(const std::vector<MapEntry>& maps, const std::string& server_zip,
                           const std::string& display_name);

MenuResult run_main_menu(SDL_Window* window, bf2::Renderer& renderer, Settings& settings,
                         const std::vector<MapEntry>& maps);

PauseResult run_pause_overlay(SDL_Window* window, bf2::Renderer& renderer, Settings& settings,
                              int& screen_w, int& screen_h, bool* options_open);

bool run_options_panel(SDL_Window* window, bf2::Renderer& renderer, Settings& settings, int& screen_w,
                       int& screen_h, bool modal);

void apply_graphics_settings(bf2::Renderer& renderer, Settings& settings);

}  // namespace dalian
