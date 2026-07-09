#pragma once

#include <SDL.h>

#include <memory>
#include <vector>

#include "app_settings.hpp"
#include "engine/net/net.hpp"
#include "engine/render/renderer.hpp"
#include "menu.hpp"

namespace dalian {

// Runs host/join browser + lobby until the match starts or the user cancels.
// On success sets result.action = StartMultiplayer and transfers net ownership.
bool run_multiplayer_flow(SDL_Window* window, bf2::Renderer& renderer, Settings& settings,
                          const std::vector<MapEntry>& maps, MenuResult& result);

}  // namespace dalian
