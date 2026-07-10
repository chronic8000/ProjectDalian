#pragma once

#include "game_state.hpp"
#include "conquest_sim.hpp"
#include "game_snapshot.hpp"
#include "map_conquest_parser.hpp"
#include "projectile_profile.hpp"
#include "weapon_profile.hpp"
#include "soldier_anim.hpp"

#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/nav/bf2_nav_mesh.hpp"
#include "engine/physics/physics_world.hpp"

#include <vector>

namespace dalian {

struct SimInitParams {
  bf2::PhysicsWorld* world = nullptr;
  float water_y = -1e9f;
  glm::vec3 spawn{};
  std::vector<glm::vec3> control_points;
  const bf2::Skeleton* soldier_ske = nullptr;
  const bf2::AnimationClip* clip_stand = nullptr;
  const bf2::AnimationClip* clip_walk = nullptr;
  const bf2::AnimationClip* clip_run = nullptr;
  bool have_soldier = false;
  bool have_clip_stand = false;
  bool have_clip_walk = false;
  bool have_clip_run = false;
  const bf2::NavMesh* nav_mesh = nullptr;
  WeaponProfile weapon{};
  ProjectileProfile sam_missile{};
  ProjectileProfile at_missile{};
  WeaponProfile enemy_weapon{};
  SoldierAnimSet soldier_anims{};
  bool missile_headless_demo = false;
  int starting_tickets = 150;
  MapConquestLayout map_layout{};
  int team1_faction_id = 0;
  int team2_faction_id = 3;
  bool bots_enabled = true;
  int bot_count = 28;
  int bot_difficulty = 3;  // 1..5
  bool multiplayer = false;
  int connected_humans = 1;
  std::string player_label = "You";
  std::vector<std::string> bot_names;
};

struct EnemyHit {
  int idx = -1;
  int zone = 0;
  float dist = 1e30f;
  glm::vec3 point{};
};

// Authoritative simulation tick — no SDL, OpenGL, or audio.
class GameSim {
public:
  static constexpr float kFixedDt = 1.f / 60.f;

  void init(const SimInitParams& params);
  void tick(float frame_dt, const PlayerInput& input);
  void begin_match();
  void restart_round();
  void set_match_factions(int team1_faction, int team2_faction, TeamId player_team);
  void set_connected_humans(int count);
  void set_weapon_profile(const WeaponProfile& weapon, bool refill_ammo = true);
  void set_at_missile_profile(const ProjectileProfile& profile);

  const GameState& state() const { return state_; }
  GameState& state() { return state_; }
  const SimEvents& events() const { return events_; }

  EnemyHit raycast_enemies(const glm::vec3& o, const glm::vec3& dir, float maxd) const;
  void apply_explosion(const glm::vec3& center, float radius, float max_damage);
  void apply_enemy_damage(int idx, int zone);
  void hurt_player(float damage, int killer_enemy_idx = -1, const char* killer_override = nullptr);
  bool can_team_spawn_at(const glm::vec3& pos, TeamId team, float epsilon = 2.5f) const;
  bool can_team_spawn_at_cp(int bf2_cp_id, TeamId team) const;
  void sync_vehicle_transform(Vehicle& v) const { rebuild_vehicle_model(v); }
  snapshot::GameState build_snapshot(std::uint32_t local_player_id = 1,
                                     float player_yaw_deg = 0.f) const;

private:
  SimInitParams params_{};
  GameState state_{};
  SimEvents events_{};
  float time_accumulator_ = 0.f;
  int heli_pod_side_ = 0;
  ConquestConfig conquest_cfg_{};
  bool defenders_spawned_ = false;

  void clear_events();
  void tick_fixed(float dt, const PlayerInput& input);
  void init_conquest();
  void spawn_defenders();
  void push_kill_feed(const std::string& killer, const std::string& victim,
                      const std::string& weapon);
  std::string player_weapon_label() const;
  std::string enemy_weapon_label() const;
  void decay_sticks(float dt, const PlayerInput& input);
  void step_vehicle_interaction(const PlayerInput& input);
  void step_rotor_spool(float dt, const PlayerInput& input);
  void step_vehicles(float dt, const PlayerInput& input);
  void step_vehicle_fatal_collisions();
  void step_player_on_foot(float dt, const PlayerInput& input);
  void step_push_player_from_hulls();
  void step_combat(float dt, const PlayerInput& input);
  void step_heli_weapons(float dt, const PlayerInput& input);
  void step_missiles(float dt, const PlayerInput& input);
  void step_projectiles(float dt);
  void step_enemies(float dt, const PlayerInput& input);
  void step_conquest(float dt);
  void decay_effects(float dt);

  void update_capsules(Enemy& en, const bf2::AnimationClip* clip, int frame) const;
  EnemyHit shoot_enemies(const glm::vec3& o, const glm::vec3& dir, float maxd) const;
  void damage_enemy(int idx, int zone);
  void damage_enemy(int idx, int zone, float weapon_damage_override);
  void explode_at(const glm::vec3& center, float radius, float max_damage, bool spawn_fx = true);
  void fire_heli_rocket(const glm::vec3& origin, const glm::vec3& dir,
                        const VehicleWeaponSlot* weapon_slot = nullptr);
  void fire_vehicle_projectile(const glm::vec3& origin, const glm::vec3& dir,
                               const VehicleWeaponSlot& weapon_slot);
  void spawn_missile_from_profile(const glm::vec3& origin, const glm::vec3& dir,
                                  const ProjectileProfile& profile, bool homing_target);

  float ground_surface(float x, float z, float refy) const;
  bool push_out_of_vehicles(float& x, float& z, float feet_y, int ignore) const;
  bool is_trapped_spawn(float x, float z, float refy, float& out_y) const;
  glm::vec3 find_safe_spawn(const glm::vec3& desired) const;
  void snap_enemy_feet(Enemy& en) const;
  bool move_vehicle_horiz(Vehicle& v, const glm::vec3& delta) const;
  float air_floor_y(const Vehicle& v) const;
  void rebuild_vehicle_model(Vehicle& v) const;
};

}  // namespace dalian
