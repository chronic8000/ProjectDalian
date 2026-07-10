#pragma once

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace dalian {

// Retail Battlefield 2 default actions (keyboard). Mouse LMB/RMB handled separately.
enum class GameAction : std::uint16_t {
  MoveForward = 0,
  MoveBack,
  StrafeLeft,
  StrafeRight,
  Jump,
  Crouch,
  Prone,
  Sprint,
  EnterExit,
  Reload,
  PickupKit,
  CycleWeapon,
  FireModeToggle,
  Parachute,
  DeployScreen,
  Scoreboard,
  CommoRose,
  ChatAll,
  ChatTeam,
  ChatSquad,
  PushToTalk,
  CommanderChannel,
  SecondaryRadio,
  VoteYes,
  VoteNo,
  SquadScreen,
  MinimapZoom,
  Screenshot,
  CycleCamera,
  WeaponSlot1,
  WeaponSlot2,
  WeaponSlot3,
  WeaponSlot4,
  WeaponSlot5,
  WeaponSlot6,
  SmokeFlares,
  Seat1,
  Seat2,
  Seat3,
  Seat4,
  Seat5,
  Seat6,
  // Project Dalian extras (not retail BF2 defaults — shown in Options separately).
  ReconDrone,
  KamikazeDrone,
  MedkitHeal,
  CarSam,  // opens tactical map; click destination to fire car-launched Igla
  Count
};

struct ActionMeta {
  GameAction action;
  const char* label;
  const char* bf2_category;  // Common / Infantry / Land / Air / Helicopter / Dalian
  bool bf2_default;          // false = Dalian-only binding
  bool implemented;          // gameplay wired (false = stub / coming soon)
  SDL_Scancode default_key;
};

constexpr std::size_t kGameActionCount = static_cast<std::size_t>(GameAction::Count);

class KeyBindings {
public:
  KeyBindings() { reset_bf2_defaults(); }

  void reset_bf2_defaults();
  void load_scancode(GameAction action, SDL_Scancode sc);
  SDL_Scancode scancode(GameAction action) const;
  bool load_kv(const std::string& key, const std::string& val);
  void save_all(std::ostream& out) const;

  static const ActionMeta& meta(GameAction action);
  static std::string scancode_label(SDL_Scancode sc);
  static SDL_Scancode scancode_from_name(std::string_view name);

private:
  std::array<SDL_Scancode, kGameActionCount> keys_{};
};

// Per-frame action state derived from SDL events + keyboard.
class InputRouter {
public:
  void begin_frame();
  void handle_event(const SDL_Event& e, const KeyBindings& bindings, bool ui_captures_keyboard);
  void poll_keyboard(const Uint8* keys, const KeyBindings& bindings);

  bool down(GameAction a) const;
  bool edge(GameAction a) const;  // true once on press; clears after read
  bool consume(GameAction a);

  bool fire() const { return fire_; }
  bool alt_fire() const { return alt_fire_; }
  int weapon_wheel_delta() const { return wheel_delta_; }
  void clear_wheel_delta() { wheel_delta_ = 0; }

private:
  void set_down(GameAction a, bool pressed);

  std::array<bool, kGameActionCount> down_{};
  std::array<bool, kGameActionCount> edge_{};
  bool fire_ = false;
  bool alt_fire_ = false;
  int wheel_delta_ = 0;
};

bool key_bindings_self_test();

}  // namespace dalian
