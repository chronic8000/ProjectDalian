#include "key_bindings.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace dalian {
namespace {

const ActionMeta kMeta[] = {
    {GameAction::MoveForward, "Move forward", "Common", true, true, SDL_SCANCODE_W},
    {GameAction::MoveBack, "Move backward", "Common", true, true, SDL_SCANCODE_S},
    {GameAction::StrafeLeft, "Strafe left", "Common", true, true, SDL_SCANCODE_A},
    {GameAction::StrafeRight, "Strafe right", "Common", true, true, SDL_SCANCODE_D},
    {GameAction::Jump, "Jump", "Infantry", true, true, SDL_SCANCODE_SPACE},
    {GameAction::Crouch, "Crouch (hold)", "Infantry", true, true, SDL_SCANCODE_LCTRL},
    {GameAction::Prone, "Prone", "Infantry", true, true, SDL_SCANCODE_Z},
    {GameAction::Sprint, "Sprint", "Infantry", true, true, SDL_SCANCODE_LSHIFT},
    {GameAction::EnterExit, "Enter / exit vehicle", "Common", true, true, SDL_SCANCODE_E},
    {GameAction::Reload, "Reload", "Infantry", true, true, SDL_SCANCODE_R},
    {GameAction::PickupKit, "Pick up kit", "Infantry", true, false, SDL_SCANCODE_G},
    {GameAction::CycleWeapon, "Cycle weapon (mouse wheel)", "Infantry", true, true,
     SDL_SCANCODE_UNKNOWN},
    {GameAction::FireModeToggle, "Toggle fire mode", "Infantry", true, false, SDL_SCANCODE_3},
    {GameAction::Parachute, "Open parachute", "Air", true, false, SDL_SCANCODE_9},
    {GameAction::DeployScreen, "Deploy / spawn screen", "Common", true, true, SDL_SCANCODE_RETURN},
    {GameAction::Scoreboard, "Scoreboard (hold)", "Common", true, true, SDL_SCANCODE_TAB},
    {GameAction::CommoRose, "Commo rose (hold)", "Common", true, false, SDL_SCANCODE_Q},
    {GameAction::ChatAll, "Chat — all", "Common", true, false, SDL_SCANCODE_J},
    {GameAction::ChatTeam, "Chat — team", "Common", true, false, SDL_SCANCODE_K},
    {GameAction::ChatSquad, "Chat — squad", "Common", true, false, SDL_SCANCODE_L},
    {GameAction::PushToTalk, "Push to talk (squad)", "Common", true, false, SDL_SCANCODE_B},
    {GameAction::CommanderChannel, "Commander channel", "Common", true, false, SDL_SCANCODE_V},
    {GameAction::SecondaryRadio, "Secondary radio", "Common", true, false, SDL_SCANCODE_T},
    {GameAction::VoteYes, "Vote yes", "Common", true, false, SDL_SCANCODE_PAGEUP},
    {GameAction::VoteNo, "Vote no", "Common", true, false, SDL_SCANCODE_PAGEDOWN},
    {GameAction::SquadScreen, "Squad screen", "Common", true, false, SDL_SCANCODE_CAPSLOCK},
    {GameAction::MinimapZoom, "Zoom minimap", "Common", true, false, SDL_SCANCODE_N},
    {GameAction::Screenshot, "Screenshot", "Common", true, false, SDL_SCANCODE_PRINTSCREEN},
    {GameAction::CycleCamera, "Cycle camera view", "Common", true, true, SDL_SCANCODE_C},
    {GameAction::WeaponSlot1, "Weapon slot 1", "Infantry", true, false, SDL_SCANCODE_1},
    {GameAction::WeaponSlot2, "Weapon slot 2", "Infantry", true, false, SDL_SCANCODE_2},
    {GameAction::WeaponSlot3, "Weapon slot 3 / fire mode", "Infantry", true, false, SDL_SCANCODE_3},
    {GameAction::WeaponSlot4, "Weapon slot 4 / grenade", "Infantry", true, true, SDL_SCANCODE_4},
    {GameAction::WeaponSlot5, "Weapon slot 5 / C4 place", "Infantry", true, true, SDL_SCANCODE_5},
    {GameAction::WeaponSlot6, "Weapon slot 6", "Infantry", true, false, SDL_SCANCODE_6},
    {GameAction::SmokeFlares, "C4 detonate / vehicle flares", "Land", true, true, SDL_SCANCODE_X},
    {GameAction::Seat1, "Vehicle seat 1", "Land", true, true, SDL_SCANCODE_F1},
    {GameAction::Seat2, "Vehicle seat 2", "Land", true, true, SDL_SCANCODE_F2},
    {GameAction::Seat3, "Vehicle seat 3", "Land", true, true, SDL_SCANCODE_F3},
    {GameAction::Seat4, "Vehicle seat 4", "Land", true, true, SDL_SCANCODE_F4},
    {GameAction::Seat5, "Vehicle seat 5", "Land", true, true, SDL_SCANCODE_F5},
    {GameAction::Seat6, "Vehicle seat 6", "Land", true, true, SDL_SCANCODE_F6},
    {GameAction::ReconDrone, "Recon FPV drone", "Dalian", false, true, SDL_SCANCODE_F9},
    {GameAction::KamikazeDrone, "Kamikaze drone", "Dalian", false, true, SDL_SCANCODE_F10},
    {GameAction::MedkitHeal, "Medkit self-heal", "Dalian", false, true, SDL_SCANCODE_H},
    {GameAction::CarSam, "Car SAM map / fire", "Dalian", false, true, SDL_SCANCODE_F8},
};

std::string action_key_name(GameAction a) {
  switch (a) {
    case GameAction::MoveForward: return "MoveForward";
    case GameAction::MoveBack: return "MoveBack";
    case GameAction::StrafeLeft: return "StrafeLeft";
    case GameAction::StrafeRight: return "StrafeRight";
    case GameAction::Jump: return "Jump";
    case GameAction::Crouch: return "Crouch";
    case GameAction::Prone: return "Prone";
    case GameAction::Sprint: return "Sprint";
    case GameAction::EnterExit: return "EnterExit";
    case GameAction::Reload: return "Reload";
    case GameAction::PickupKit: return "PickupKit";
    case GameAction::CycleWeapon: return "CycleWeapon";
    case GameAction::FireModeToggle: return "FireModeToggle";
    case GameAction::Parachute: return "Parachute";
    case GameAction::DeployScreen: return "DeployScreen";
    case GameAction::Scoreboard: return "Scoreboard";
    case GameAction::CommoRose: return "CommoRose";
    case GameAction::ChatAll: return "ChatAll";
    case GameAction::ChatTeam: return "ChatTeam";
    case GameAction::ChatSquad: return "ChatSquad";
    case GameAction::PushToTalk: return "PushToTalk";
    case GameAction::CommanderChannel: return "CommanderChannel";
    case GameAction::SecondaryRadio: return "SecondaryRadio";
    case GameAction::VoteYes: return "VoteYes";
    case GameAction::VoteNo: return "VoteNo";
    case GameAction::SquadScreen: return "SquadScreen";
    case GameAction::MinimapZoom: return "MinimapZoom";
    case GameAction::Screenshot: return "Screenshot";
    case GameAction::CycleCamera: return "CycleCamera";
    case GameAction::WeaponSlot1: return "WeaponSlot1";
    case GameAction::WeaponSlot2: return "WeaponSlot2";
    case GameAction::WeaponSlot3: return "WeaponSlot3";
    case GameAction::WeaponSlot4: return "WeaponSlot4";
    case GameAction::WeaponSlot5: return "WeaponSlot5";
    case GameAction::WeaponSlot6: return "WeaponSlot6";
    case GameAction::SmokeFlares: return "SmokeFlares";
    case GameAction::Seat1: return "Seat1";
    case GameAction::Seat2: return "Seat2";
    case GameAction::Seat3: return "Seat3";
    case GameAction::Seat4: return "Seat4";
    case GameAction::Seat5: return "Seat5";
    case GameAction::Seat6: return "Seat6";
    case GameAction::ReconDrone: return "ReconDrone";
    case GameAction::KamikazeDrone: return "KamikazeDrone";
    case GameAction::MedkitHeal: return "MedkitHeal";
    case GameAction::CarSam: return "CarSam";
    default: return "Unknown";
  }
}

}  // namespace

void InputRouter::set_down(GameAction a, bool pressed) {
  const std::size_t i = static_cast<std::size_t>(a);
  if (pressed && !down_[i]) edge_[i] = true;
  down_[i] = pressed;
}

void KeyBindings::reset_bf2_defaults() {
  for (const auto& m : kMeta) {
    keys_[static_cast<std::size_t>(m.action)] = m.default_key;
  }
}

void KeyBindings::load_scancode(GameAction action, SDL_Scancode sc) {
  if (sc == SDL_SCANCODE_UNKNOWN) return;
  keys_[static_cast<std::size_t>(action)] = sc;
}

SDL_Scancode KeyBindings::scancode(GameAction action) const {
  return keys_[static_cast<std::size_t>(action)];
}

bool KeyBindings::load_kv(const std::string& key, const std::string& val) {
  if (key.rfind("bind_", 0) != 0) return false;
  const std::string name = key.substr(5);
  for (const auto& m : kMeta) {
    if (name != action_key_name(m.action)) continue;
    const SDL_Scancode sc = scancode_from_name(val);
    if (sc != SDL_SCANCODE_UNKNOWN) load_scancode(m.action, sc);
    return true;
  }
  return false;
}

void KeyBindings::save_all(std::ostream& out) const {
  for (const auto& m : kMeta) {
    const SDL_Scancode sc = scancode(m.action);
    out << "bind_" << action_key_name(m.action) << '=' << static_cast<int>(sc) << '\n';
  }
}

const ActionMeta& KeyBindings::meta(GameAction action) {
  return kMeta[static_cast<std::size_t>(action)];
}

std::string KeyBindings::scancode_label(SDL_Scancode sc) {
  if (sc == SDL_SCANCODE_UNKNOWN) return "Unbound";
  const char* n = SDL_GetScancodeName(sc);
  if (n == nullptr || n[0] == '\0') return "?";
  std::string s = n;
  if (s.size() == 1) return s;
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

SDL_Scancode KeyBindings::scancode_from_name(std::string_view name) {
  if (name.empty()) return SDL_SCANCODE_UNKNOWN;
  bool all_digits = true;
  for (char c : name) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      all_digits = false;
      break;
    }
  }
  if (all_digits) {
    const int v = std::atoi(std::string(name).c_str());
    if (v > 0 && v < SDL_NUM_SCANCODES) return static_cast<SDL_Scancode>(v);
  }
  std::string lower(name);
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return SDL_GetScancodeFromName(lower.c_str());
}

void InputRouter::begin_frame() {
  edge_.fill(false);
  wheel_delta_ = 0;
}

void InputRouter::handle_event(const SDL_Event& e, const KeyBindings& bindings,
                               bool ui_captures_keyboard) {
  if (ui_captures_keyboard) return;
  if (e.type == SDL_KEYDOWN) {
    const SDL_Scancode sc = e.key.keysym.scancode;
    for (std::size_t i = 0; i < kGameActionCount; ++i) {
      if (bindings.scancode(static_cast<GameAction>(i)) == sc) {
        set_down(static_cast<GameAction>(i), true);
      }
    }
  } else if (e.type == SDL_KEYUP) {
    const SDL_Scancode sc = e.key.keysym.scancode;
    for (std::size_t i = 0; i < kGameActionCount; ++i) {
      if (bindings.scancode(static_cast<GameAction>(i)) == sc) {
        set_down(static_cast<GameAction>(i), false);
      }
    }
  } else if (e.type == SDL_MOUSEBUTTONDOWN) {
    if (e.button.button == SDL_BUTTON_LEFT) fire_ = true;
    if (e.button.button == SDL_BUTTON_RIGHT) alt_fire_ = true;
  } else if (e.type == SDL_MOUSEBUTTONUP) {
    if (e.button.button == SDL_BUTTON_LEFT) fire_ = false;
    if (e.button.button == SDL_BUTTON_RIGHT) alt_fire_ = false;
  } else if (e.type == SDL_MOUSEWHEEL) {
    wheel_delta_ += e.wheel.y;
  }
}

void InputRouter::poll_keyboard(const Uint8* keys, const KeyBindings& bindings) {
  if (keys == nullptr) return;
  for (std::size_t i = 0; i < kGameActionCount; ++i) {
    const GameAction a = static_cast<GameAction>(i);
    const SDL_Scancode sc = bindings.scancode(a);
    if (sc == SDL_SCANCODE_UNKNOWN) continue;
    const bool pressed = keys[sc] != 0;
    set_down(a, pressed);
  }
}

bool InputRouter::down(GameAction a) const { return down_[static_cast<std::size_t>(a)]; }

bool InputRouter::edge(GameAction a) const { return edge_[static_cast<std::size_t>(a)]; }

bool InputRouter::consume(GameAction a) {
  const std::size_t i = static_cast<std::size_t>(a);
  const bool e = edge_[i];
  edge_[i] = false;
  return e;
}

bool key_bindings_self_test() {
  KeyBindings kb;
  if (kb.scancode(GameAction::MoveForward) != SDL_SCANCODE_W) return false;
  if (kb.scancode(GameAction::PickupKit) != SDL_SCANCODE_G) return false;
  if (KeyBindings::scancode_label(SDL_SCANCODE_SPACE) != "SPACE") return false;
  std::ostringstream oss;
  kb.save_all(oss);
  KeyBindings kb2;
  std::istringstream iss(oss.str());
  std::string line;
  while (std::getline(iss, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    kb2.load_kv(line.substr(0, eq), line.substr(eq + 1));
  }
  return kb2.scancode(GameAction::Reload) == SDL_SCANCODE_R;
}

}  // namespace dalian
