#include "menu_music.hpp"

#include "app_settings.hpp"
#include "audio_context.hpp"

#include <SDL.h>
#include <SDL_mixer.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace dalian {
namespace {

std::string user_downloads_path(const char* filename) {
  const char* home = std::getenv("USERPROFILE");
  if (!home) home = std::getenv("HOME");
  if (!home) return {};
  return (std::filesystem::path(home) / "Downloads" / filename).string();
}

std::string user_downloads_dalian_menu() {
  return user_downloads_path("The_Siege_of_Dalian.mp3");
}

std::string user_downloads_loading() {
  return user_downloads_path("Before_the_First_Volley.mp3");
}

std::string exe_directory() {
  char* base = SDL_GetBasePath();
  if (!base) return {};
  std::string dir = base;
  SDL_free(base);
  return dir;
}

}  // namespace

std::string resolve_menu_music_path(const Settings& settings) {
  if (!settings.menu_music.empty() && std::filesystem::exists(settings.menu_music))
    return settings.menu_music;

  if (const char* env = std::getenv("BF2_MENU_MUSIC")) {
    if (std::filesystem::exists(env)) return env;
  }

  const std::string downloads = user_downloads_dalian_menu();
  if (!downloads.empty() && std::filesystem::exists(downloads)) return downloads;

  const std::string exe = exe_directory();
  if (!exe.empty()) {
    const std::string next_to_exe = (std::filesystem::path(exe) / "music" / "The_Siege_of_Dalian.mp3").string();
    if (std::filesystem::exists(next_to_exe)) return next_to_exe;
  }

  char* pref = SDL_GetPrefPath("ProjectDalian", "ProjectDalian");
  if (pref) {
    const std::string pref_music =
        (std::filesystem::path(pref) / "music" / "The_Siege_of_Dalian.mp3").string();
    SDL_free(pref);
    if (std::filesystem::exists(pref_music)) return pref_music;
  }

  return {};
}

std::string resolve_loading_music_path(const Settings& settings) {
  if (const char* env = std::getenv("BF2_LOADING_MUSIC")) {
    if (std::filesystem::exists(env)) return env;
  }

  const std::string downloads = user_downloads_loading();
  if (!downloads.empty() && std::filesystem::exists(downloads)) return downloads;

  const std::string exe = exe_directory();
  if (!exe.empty()) {
    const std::string next_to_exe =
        (std::filesystem::path(exe) / "music" / "Before_the_First_Volley.mp3").string();
    if (std::filesystem::exists(next_to_exe)) return next_to_exe;
  }

  char* pref = SDL_GetPrefPath("ProjectDalian", "ProjectDalian");
  if (pref) {
    const std::string pref_music =
        (std::filesystem::path(pref) / "music" / "Before_the_First_Volley.mp3").string();
    SDL_free(pref);
    if (std::filesystem::exists(pref_music)) return pref_music;
  }

  return {};
}

bool MenuMusic::init() {
  if (open_) return true;
  if (!AudioContext::acquire()) return false;
  open_ = true;
  ready_ = true;
  return true;
}

void MenuMusic::shutdown() {
  stop();
  if (music_) {
    Mix_FreeMusic(static_cast<Mix_Music*>(music_));
    music_ = nullptr;
  }
  if (open_) {
    AudioContext::release();
    open_ = false;
  }
  ready_ = false;
}

bool MenuMusic::play_file(const std::string& path, float volume, int loops) {
  if (!ready_ || path.empty()) return false;
  stop();
  if (music_) {
    Mix_FreeMusic(static_cast<Mix_Music*>(music_));
    music_ = nullptr;
  }
  music_ = Mix_LoadMUS(path.c_str());
  if (!music_) {
    std::cerr << "MenuMusic: failed to load " << path << ": " << Mix_GetError() << '\n';
    return false;
  }
  Mix_VolumeMusic(static_cast<int>(std::clamp(volume, 0.f, 1.f) * MIX_MAX_VOLUME));
  if (Mix_PlayMusic(static_cast<Mix_Music*>(music_), loops) != 0) {
    std::cerr << "MenuMusic: Mix_PlayMusic failed: " << Mix_GetError() << '\n';
    Mix_FreeMusic(static_cast<Mix_Music*>(music_));
    music_ = nullptr;
    return false;
  }
  std::cout << "MenuMusic: playing " << path << (loops == 0 ? " (once)" : "") << '\n';
  return true;
}

void MenuMusic::set_volume(float volume) {
  if (!open_) return;
  Mix_VolumeMusic(static_cast<int>(std::clamp(volume, 0.f, 1.f) * MIX_MAX_VOLUME));
}

void MenuMusic::stop() {
  if (!open_) return;
  Mix_HaltMusic();
}

bool MenuMusic::playing() const {
  return open_ && Mix_PlayingMusic() != 0;
}

}  // namespace dalian
