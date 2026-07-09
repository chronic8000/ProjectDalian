#pragma once

#include "conquest_types.hpp"
#include "game_sim_types.hpp"
#include "soldier_anim.hpp"

#include "engine/physics/physics_world.hpp"

#include <vector>

namespace dalian {

// Dense simulation snapshot — suitable for net replication and headless ticks.
struct GameState {
  std::vector<Vehicle> vehicles;
  std::vector<Enemy> enemies;
  std::vector<Tracer> tracers;
  std::vector<Impact> impacts;
  std::vector<Projectile> projectiles;
  std::vector<ActiveMissile> missiles;
  std::vector<Smoke> smoke;
  std::vector<Explosion> explosions;
  std::vector<Flare> flares;

  bf2::CharacterController player{};

  int in_vehicle = -1;
  int player_seat = 0;
  float air_pitch_stick = 0.f;
  float air_roll_stick = 0.f;
  float air_input_grace = 0.f;

  float fire_cooldown = 0.f;
  float muzzle_flash = 0.f;
  float recoil = 0.f;
  bool ballistic = true;

  float missile_reload = 0.f;
  float heli_rocket_cd = 0.f;
  float heli_gun_cd = 0.f;
  float heli_grocket_cd = 0.f;
  float heli_flare_cd = 0.f;
  int gunner_target = -1;
  float gunner_acquire = 0.f;
  bool gunner_engaging = false;

  int player_kills = 0;
  int player_deaths = 0;
  float player_health = 100.f;
  float player_regen_delay = 0.f;
  float player_stamina = 100.f;
  SoldierPose infantry_pose = SoldierPose::Stand;

  int mag_ammo = 30;
  int reserve_ammo = 90;
  bool reloading = false;
  float reload_timer = 0.f;
  float fire_deviation = 0.02f;
  int shots_fired = 0;
  int burst_shots_left = 0;
  float burst_pause_timer = 0.f;
  bool fire_was_down = false;

  glm::vec3 wind_base{2.6f, 0.f, 1.4f};
  glm::vec3 wind{2.6f, 0.f, 1.4f};

  std::vector<ControlPoint> control_points;
  TicketState tickets{};
  float round_time = 0.f;
  TeamId winning_team = TeamId::Neutral;
  bool match_over = false;
  bool match_started = false;
  bool team1_ticket_warned = false;
  bool team2_ticket_warned = false;
  TeamId player_team = TeamId::Team1;
  int team1_faction_id = 0;
  int team2_faction_id = 3;
};

// Side-effects the client handles (audio, net, UI transitions).
struct SimEvents {
  bool fired_shot = false;
  glm::vec3 fire_origin{};
  glm::vec3 fire_dir{};
  bool open_deploy = false;
  bool out_of_ammo_voice = false;
  bool play_reload = false;
  bool capture_mouse = false;
  bool discard_mouse_delta = false;
  bool vehicle_exited = false;
  float exit_yaw_deg = 0.f;
  bool match_over = false;
  TeamId winning_team = TeamId::Neutral;
  bool control_point_captured = false;
  std::uint16_t captured_cp_id = 0;
  TeamId captured_cp_owner = TeamId::Neutral;
  std::string voice_cue;  // semantic cue → conquest_voice.hpp
};

}  // namespace dalian
