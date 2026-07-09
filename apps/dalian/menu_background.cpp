#include "menu_background.hpp"

#include <SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <filesystem>
#include <iostream>

namespace dalian {
namespace {

constexpr const char* kBackgroundFile = "dalian_plant_background.png";

std::string exe_directory() {
  char* base = SDL_GetBasePath();
  if (!base) return {};
  std::string dir = base;
  SDL_free(base);
  return dir;
}

}  // namespace

std::string resolve_menu_background_path() {
  if (const char* env = std::getenv("BF2_MENU_BACKGROUND")) {
    if (std::filesystem::exists(env)) return env;
  }

  const std::string exe = exe_directory();
  if (!exe.empty()) {
    const std::string next_to_exe =
        (std::filesystem::path(exe) / "menu" / kBackgroundFile).string();
    if (std::filesystem::exists(next_to_exe)) return next_to_exe;
  }

  char* pref = SDL_GetPrefPath("ProjectDalian", "ProjectDalian");
  if (pref) {
    const std::string pref_bg =
        (std::filesystem::path(pref) / "menu" / kBackgroundFile).string();
    SDL_free(pref);
    if (std::filesystem::exists(pref_bg)) return pref_bg;
  }

  return {};
}

bool MenuBackground::load(bf2::Renderer& renderer) {
  destroy(renderer);
  const std::string path = resolve_menu_background_path();
  if (path.empty()) {
    std::cerr << "MenuBackground: " << kBackgroundFile << " not found (check menu/ folder)\n";
    return false;
  }

  int w = 0, h = 0, comp = 0;
  stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (!pixels || w <= 0 || h <= 0) {
    std::cerr << "MenuBackground: failed to load " << path << '\n';
    stbi_image_free(pixels);
    return false;
  }

  texture = renderer.upload_rgba_texture(w, h, pixels);
  stbi_image_free(pixels);
  if (texture == 0) return false;

  width = w;
  height = h;
  std::cout << "MenuBackground: loaded " << path << " (" << w << "x" << h << ")\n";
  return true;
}

void MenuBackground::destroy(bf2::Renderer& renderer) {
  if (texture != 0) {
    renderer.destroy_texture(texture);
    texture = 0;
  }
  width = 0;
  height = 0;
}

void MenuBackground::draw(bf2::Renderer& renderer, float dim_alpha) const {
  if (texture == 0) return;
  renderer.ui_image_cover(texture, width, height, 1.f);
  if (dim_alpha > 0.f) renderer.ui_dim_framebuffer(dim_alpha);
}

}  // namespace dalian
