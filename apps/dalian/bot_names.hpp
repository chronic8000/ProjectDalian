#pragma once

#include <string>
#include <vector>

namespace dalian {

// Retail BF2 bot roster from mods/bf2/AI/BotNames.ai (aiSettings.addBotName ...).
std::vector<std::string> load_bot_names(const std::string& bf2_root);

std::string pick_bot_name(const std::vector<std::string>& names, std::size_t index);

}  // namespace dalian
