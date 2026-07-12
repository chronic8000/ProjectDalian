#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "app_settings.hpp"

namespace dalian {

// True when path looks like a BF2 install root (contains a mods/ folder).
bool is_valid_bf2_root(const std::string& path);

// If the user picked mods/ or a nested EA folder, climb to the real root.
// Returns empty string when the path is not usable.
std::string normalize_bf2_root(std::string path);

// Candidate install locations: classic Program Files, EA App / Origin defaults,
// registry Install Dir, and common D:/E: game folders.
std::vector<std::string> bf2_install_candidates();

// Native Windows folder picker (IFileOpenDialog). parent may be null.
// Returns empty if cancelled / unavailable.
std::string browse_bf2_install_folder(SDL_Window* window);

// Resolve + persist bf2_root: settings → argv map path → auto-scan candidates.
std::string resolve_bf2_root(Settings& settings, const char* argv1);

// Apply a user-chosen path (browse / paste). Saves settings when valid.
// Returns true if settings.bf2_root was updated to a valid root.
bool apply_bf2_root(Settings& settings, const std::string& chosen);

}  // namespace dalian
