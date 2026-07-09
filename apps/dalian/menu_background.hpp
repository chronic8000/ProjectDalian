#pragma once

#include "engine/render/renderer.hpp"

#include <cstdint>
#include <string>

namespace dalian {

struct MenuBackground {
  std::uint32_t texture = 0;
  int width = 0;
  int height = 0;

  bool load(bf2::Renderer& renderer);
  void destroy(bf2::Renderer& renderer);
  void draw(bf2::Renderer& renderer, float dim_alpha = 0.42f) const;
};

std::string resolve_menu_background_path();

}  // namespace dalian
