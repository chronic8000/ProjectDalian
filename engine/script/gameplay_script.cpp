#include "gameplay_script.hpp"

#include <algorithm>

namespace bf2 {

bool GameplayScript::initialize() { return true; }

void GameplayScript::shutdown() { state_.clear(); }

void GameplayScript::bind_float(const std::string& name, float value) { state_[name] = value; }

void GameplayScript::call(const std::string& function) {
  if (function == "fire_weapon") {
    state_["ammo"] = std::max(0.f, state_["ammo"] - 1.f);
  }
}

float GameplayScript::get_float(const std::string& name) const {
  if (auto it = state_.find(name); it != state_.end()) {
    return it->second;
  }
  return 0.f;
}

}  // namespace bf2
