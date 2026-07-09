#pragma once

#include <string>

namespace dalian {

struct Settings;

// Looping background music for the main menu (SDL_mixer).
class MenuMusic {
 public:
  bool init();
  void shutdown();
  bool play_file(const std::string& path, float volume, int loops = -1);
  void set_volume(float volume);
  void stop();
  bool playing() const;

 private:
  bool ready_ = false;
  bool open_ = false;
  void* music_ = nullptr;  // Mix_Music*
};

std::string resolve_menu_music_path(const Settings& settings);
std::string resolve_loading_music_path(const Settings& settings);

}  // namespace dalian
