#include "bot_names.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dalian {
namespace {

void parse_bot_names_file(const std::filesystem::path& path, std::vector<std::string>& out) {
  if (!std::filesystem::exists(path)) return;
  std::ifstream in(path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find("addBotName");
    if (pos == std::string::npos) continue;
    if (line.rfind("rem", 0) == 0) continue;
    std::string name = line.substr(pos + 10);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    if (!name.empty()) out.push_back(name);
  }
}

}  // namespace

std::vector<std::string> load_bot_names(const std::string& bf2_root) {
  std::vector<std::string> names;
  if (bf2_root.empty()) return names;

  const std::filesystem::path root(bf2_root);
  parse_bot_names_file(root / "mods" / "bf2" / "AI" / "BotNames.ai", names);
  parse_bot_names_file(root / "mods" / "xpack" / "AI" / "BotNames.ai", names);
  parse_bot_names_file(root / "mods" / "bf264" / "AI" / "BotNames.ai", names);

  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  if (!names.empty()) {
    std::cout << "BotNames: loaded " << names.size() << " retail BF2 names\n";
  } else {
    std::cerr << "BotNames: BotNames.ai not found under " << bf2_root << '\n';
  }
  return names;
}

std::string pick_bot_name(const std::vector<std::string>& names, std::size_t index) {
  if (names.empty()) {
    char fb[16];
    std::snprintf(fb, sizeof(fb), "Bot %zu", (index % 999) + 1);
    return fb;
  }
  return names[index % names.size()];
}

}  // namespace dalian
