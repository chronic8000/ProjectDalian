#include "game_sim.hpp"

#include "bot_names.hpp"

#include <cstring>

#include "bf2_effects.hpp"
#include "hitbox_zones.hpp"

#include "engine/anim/pose.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace dalian {
namespace {

constexpr int kMagSize = 30;
constexpr float kMuzzleSpeed = 340.f;
constexpr float kBulletGravity = -9.81f;
constexpr float kBulletLife = 3.0f;
constexpr float kBulletDrag = 0.0011f;

struct BonePair {
  int a, b;
  float r;
  int zone;
};
constexpr BonePair kBonePairs[] = {
    {46, 47, 0.14f, 2}, {13, 46, 0.19f, 1}, {11, 13, 0.21f, 1}, {0, 11, 0.19f, 1},
    {1, 3, 0.12f, 0},  {6, 8, 0.12f, 0},  {3, 4, 0.09f, 0},  {8, 9, 0.09f, 0},
    {15, 16, 0.09f, 0}, {31, 32, 0.09f, 0}, {16, 20, 0.07f, 0}, {32, 35, 0.07f, 0},
};

float closest_seg_seg_sq(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2,
                         const glm::vec3& q2, float& s) {
  const glm::vec3 d1 = q1 - p1;
  const glm::vec3 d2 = q2 - p2;
  const glm::vec3 r = p1 - p2;
  const float a = glm::dot(d1, d1);
  const float e = glm::dot(d2, d2);
  const float f = glm::dot(d2, r);
  float t;
  if (a <= 1e-8f && e <= 1e-8f) {
    s = 0.f;
    return glm::dot(r, r);
  }
  if (a <= 1e-8f) {
    s = 0.f;
    t = std::clamp(f / e, 0.f, 1.f);
  } else {
    const float c = glm::dot(d1, r);
    if (e <= 1e-8f) {
      t = 0.f;
      s = std::clamp(-c / a, 0.f, 1.f);
    } else {
      const float b = glm::dot(d1, d2);
      const float denom = a * e - b * b;
      s = denom > 1e-8f ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
      t = (b * s + f) / e;
      if (t < 0.f) {
        t = 0.f;
        s = std::clamp(-c / a, 0.f, 1.f);
      } else if (t > 1.f) {
        t = 1.f;
        s = std::clamp((b - c) / a, 0.f, 1.f);
      }
    }
  }
  const glm::vec3 c1 = p1 + d1 * s;
  const glm::vec3 c2 = p2 + d2 * t;
  return glm::dot(c1 - c2, c1 - c2);
}

float frand() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); }

}  // namespace

void GameSim::init(const SimInitParams& params) {
  params_ = params;
  state_ = GameState{};
  defenders_spawned_ = false;
  conquest_cfg_.starting_tickets = params_.starting_tickets;
  state_.tickets.team1_tickets = params_.starting_tickets;
  state_.tickets.team2_tickets = params_.starting_tickets;
  state_.player.eye_height = 1.8f;
  if (params_.spawn.y > 0.f || glm::length(glm::vec2(params_.spawn.x, params_.spawn.z)) > 0.01f) {
    state_.player.position = {params_.spawn.x, params_.spawn.y, params_.spawn.z};
  }
  init_conquest();
}

void GameSim::init_conquest() {
  state_.control_points.clear();
  state_.match_started = false;
  state_.match_over = false;
  state_.winning_team = TeamId::Neutral;
  state_.round_time = 0.f;
  state_.team1_ticket_warned = false;
  state_.team2_ticket_warned = false;
  state_.tickets.team1_bleed_accum = 0.f;
  state_.tickets.team2_bleed_accum = 0.f;
  state_.team1_faction_id = params_.team1_faction_id;
  state_.team2_faction_id = params_.team2_faction_id;

  const auto& layout = params_.map_layout;
  if (!layout.control_points.empty()) {
    for (const auto& pcp : layout.control_points) {
      ControlPoint cp;
      cp.id = static_cast<std::uint16_t>(state_.control_points.size() + 1);
      cp.bf2_cp_id = pcp.bf2_id;
      cp.name = pcp.label;
      cp.pos = pcp.pos;
      cp.owner = pcp.initial_team;
      cp.capturer = pcp.initial_team;
      cp.capture_progress = pcp.initial_team != TeamId::Neutral ? 1.f : 0.f;
      cp.radius = pcp.radius > 0.1f ? pcp.radius : conquest_cfg_.capture_radius;
      cp.area_value_team1 = pcp.area_value_team1;
      cp.area_value_team2 = pcp.area_value_team2;
      cp.capturable = !pcp.unable_to_change_team;
      state_.control_points.push_back(cp);
    }
    return;
  }

  static const char* kNames[] = {"ALPHA",   "BRAVO", "CHARLIE", "DELTA", "ECHO", "FOXTROT",
                                 "GOLF",    "HOTEL", "INDIA",   "JULIET", "KILO", "LIMA"};
  std::size_t idx = 0;
  for (const auto& cp_pos : params_.control_points) {
    ControlPoint cp;
    cp.id = static_cast<std::uint16_t>(idx + 1);
    cp.name = kNames[idx % 12];
    cp.pos = cp_pos;
    cp.radius = conquest_cfg_.capture_radius;
    state_.control_points.push_back(cp);
    ++idx;
  }
  if (state_.control_points.empty()) return;

  int home1 = 0;
  int home2 = 0;
  float best1 = 1e30f;
  float best2 = -1.f;
  const glm::vec3 spawn = params_.spawn;
  for (std::size_t i = 0; i < state_.control_points.size(); ++i) {
    const float d2 = xz_distance_sq(state_.control_points[i].pos, spawn);
    if (d2 < best1) {
      best1 = d2;
      home1 = static_cast<int>(i);
    }
    if (d2 > best2) {
      best2 = d2;
      home2 = static_cast<int>(i);
    }
  }
  state_.control_points[home1].owner = TeamId::Team1;
  state_.control_points[home1].capturer = TeamId::Team1;
  state_.control_points[home1].capture_progress = 1.f;
  if (home2 != home1) {
    state_.control_points[home2].owner = TeamId::Team2;
    state_.control_points[home2].capturer = TeamId::Team2;
    state_.control_points[home2].capture_progress = 1.f;
  }
}

void GameSim::begin_match() {
  state_.match_started = true;
  state_.match_over = false;
  state_.winning_team = TeamId::Neutral;
  state_.round_time = 0.f;
  state_.team1_ticket_warned = false;
  state_.team2_ticket_warned = false;
  state_.tickets.team1_bleed_accum = 0.f;
  state_.tickets.team2_bleed_accum = 0.f;
  state_.tickets.team1_tickets = conquest_cfg_.starting_tickets;
  state_.tickets.team2_tickets = conquest_cfg_.starting_tickets;
  if (!defenders_spawned_) {
    spawn_defenders();
    defenders_spawned_ = true;
  }
}

void GameSim::restart_round() { begin_match(); }

void GameSim::set_connected_humans(int count) {
  params_.connected_humans = std::max(1, count);
}

void GameSim::set_match_factions(int team1_faction, int team2_faction, TeamId player_team) {
  state_.team1_faction_id = team1_faction;
  state_.team2_faction_id = team2_faction;
  state_.player_team = player_team;
}

void GameSim::step_conquest(float dt) {
#include "game_sim_conquest.inl"
}

void GameSim::clear_events() { events_ = SimEvents{}; }

std::string GameSim::player_weapon_label() const {
  if (!params_.weapon.name.empty()) return params_.weapon.name;
  return "Rifle";
}

std::string GameSim::enemy_weapon_label() const {
  if (!params_.enemy_weapon.name.empty()) return params_.enemy_weapon.name;
  return "Rifle";
}

void GameSim::push_kill_feed(const std::string& killer, const std::string& victim,
                             const std::string& weapon) {
  if (killer.empty() || victim.empty()) return;
  events_.kill_feed.push_back({killer, victim, weapon});
}

void GameSim::spawn_defenders() {
  if (!params_.have_soldier || !params_.world) return;
  if (!params_.bots_enabled || params_.bot_count <= 0) return;
  state_.enemies.clear();

  const TeamId player = state_.player_team;
  const TeamId opfor = player == TeamId::Team1 ? TeamId::Team2 : TeamId::Team1;
  constexpr float kMinFromPlayer = 70.f;
  constexpr float kMinFromFriendlyBase = 50.f;
  constexpr float kMinPostSpacing = 90.f;
  const std::size_t kMaxEnemies =
      static_cast<std::size_t>(std::clamp(params_.bot_count, 1, 128));
  const std::size_t kMaxPosts = std::max<std::size_t>(2, kMaxEnemies / 3);

  const glm::vec3 player_feet(state_.player.position.x,
                              state_.player.position.y - state_.player.eye_height,
                              state_.player.position.z);

  auto xz_dist = [](const glm::vec3& a, const glm::vec3& b) {
    return glm::distance(glm::vec2(a.x, a.z), glm::vec2(b.x, b.z));
  };

  auto too_near_player = [&](const glm::vec3& p) {
    return xz_dist(p, player_feet) < kMinFromPlayer;
  };

  auto too_near_friendly_base = [&](const glm::vec3& p) {
    for (const auto& cp : state_.control_points) {
      if (cp.owner == player && xz_dist(p, cp.pos) < kMinFromFriendlyBase) return true;
    }
    for (const auto& sp : params_.map_layout.soldier_spawns) {
      bool friendly = false;
      for (const auto& cp : state_.control_points) {
        if (cp.bf2_cp_id != 0 && cp.bf2_cp_id == sp.bf2_cp_id && cp.owner == player) {
          friendly = true;
          break;
        }
      }
      if (!friendly) {
        for (const auto& pcp : params_.map_layout.control_points) {
          if (pcp.bf2_id != 0 && pcp.bf2_id == sp.bf2_cp_id && pcp.initial_team == player) {
            friendly = true;
            break;
          }
        }
      }
      if (friendly && xz_dist(p, sp.pos) < kMinFromFriendlyBase) return true;
    }
    return false;
  };

  auto valid_defender_post = [&](const glm::vec3& pos) {
    if (too_near_player(pos)) return false;
    if (too_near_friendly_base(pos)) return false;
    if (ground_surface(pos.x, pos.z, pos.y) < params_.water_y - 0.3f) return false;
    return true;
  };

  auto cp_ok_for_defenders = [&](const ControlPoint& cp) {
    if (cp.owner == player) return false;
    if (cp.owner != opfor && cp.owner != TeamId::Neutral) return false;
    if (!cp.capturable && cp.owner == TeamId::Neutral) return false;
    return valid_defender_post(cp.pos);
  };

  std::vector<glm::vec3> candidates;
  for (const auto& cp : state_.control_points) {
    if (cp_ok_for_defenders(cp)) candidates.push_back(cp.pos);
  }
  if (candidates.empty()) {
    for (const auto& cp : params_.control_points) {
      if (valid_defender_post(cp)) candidates.push_back(cp);
    }
  }

  std::shuffle(candidates.begin(), candidates.end(), std::mt19937{std::random_device{}()});
  std::size_t bot_name_idx = 0;
  std::vector<glm::vec3> posts;
  for (const auto& cp : candidates) {
    bool too_close = false;
    for (const auto& p : posts) {
      if (xz_dist(cp, p) < kMinPostSpacing) {
        too_close = true;
        break;
      }
    }
    if (too_close) continue;
    posts.push_back(cp);
    if (posts.size() >= kMaxPosts) break;
  }
  if (posts.empty()) {
    for (int i = 0; i < 5; ++i) {
      const float ang = frand() * 6.2831853f;
      const float r = kMinFromPlayer + 20.f + frand() * 80.f;
      const glm::vec3 guess(player_feet.x + std::cos(ang) * r, 0.f,
                            player_feet.z + std::sin(ang) * r);
      if (valid_defender_post(guess)) posts.push_back(guess);
    }
  }
  for (const auto& post : posts) {
    if (state_.enemies.size() >= kMaxEnemies) break;
    const int squad = 2 + (std::rand() % 2);
    for (int i = 0; i < squad; ++i) {
      if (state_.enemies.size() >= kMaxEnemies) break;
      Enemy en;
      en.team = opfor;
      const float ang = frand() * 6.2831853f;
      const float r = 4.f + frand() * 14.f;
      en.home = post;
      en.pos = glm::vec3(post.x + std::cos(ang) * r, 0.f, post.z + std::sin(ang) * r);
      snap_enemy_feet(en);
      en.patrol_target = en.pos;
      en.patrol_wait = 1.f + frand() * 2.f;
      en.yaw = frand() * 6.2831853f;
      en.burst_cooldown = 0.5f + frand();
      if (params_.enemy_weapon.valid) {
        en.fire_rate = params_.enemy_weapon.fire_rate;
        en.damage = params_.enemy_weapon.damage;
        en.spread = params_.enemy_weapon.spread;
      }
      en.name = pick_bot_name(params_.bot_names, bot_name_idx++);
      state_.enemies.push_back(en);
    }
  }
}

float GameSim::ground_surface(float x, float z, float refy) const {
  if (!params_.world) return 0.f;
  const float terr = params_.world->terrain_height(x, z);
  if (refy > terr + 1.0f) {
    const auto dn = params_.world->raycast({x, refy + 8.f, z}, {0.f, -1.f, 0.f}, 60.f);
    if (dn.hit && std::fabs(dn.normal.y) > 0.4f && dn.point.y > terr + 1.0f &&
        dn.point.y <= refy + 8.5f) {
      return dn.point.y;
    }
  }
  return terr;
}

void GameSim::snap_enemy_feet(Enemy& en) const {
  if (!params_.world) return;
  const float terr = params_.world->terrain_height(en.pos.x, en.pos.z);
  float y = terr;
  const float ref = en.home.y;
  if (ref > terr + 1.0f) {
    const auto dn =
        params_.world->raycast({en.pos.x, ref + 8.f, en.pos.z}, {0.f, -1.f, 0.f}, 60.f);
    if (dn.hit && std::fabs(dn.normal.y) > 0.4f && dn.point.y > terr + 1.0f &&
        dn.point.y <= ref + 8.5f) {
      y = dn.point.y;
    }
  }
  en.pos.y = y;
}

bool GameSim::move_vehicle_horiz(Vehicle& v, const glm::vec3& delta) const {
  if (!params_.world) return false;
  const float dist = glm::length(glm::vec3(delta.x, 0.f, delta.z));
  if (dist < 1e-4f) {
    v.pos += delta;
    return false;
  }
  const glm::vec3 d = glm::normalize(glm::vec3(delta.x, 0.f, delta.z));
  const glm::vec3 o(v.pos.x, v.pos.y + 1.2f, v.pos.z);
  const float veh_radius = v.is_air ? 3.0f : 2.4f;
  const auto hit = params_.world->raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, dist + veh_radius);
  if (hit.hit && hit.distance < dist + veh_radius && std::fabs(hit.normal.y) < 0.6f) {
    const float back = std::max(0.f, hit.distance - veh_radius);
    v.pos += d * back;
    return true;
  }
  v.pos += delta;
  return false;
}

float GameSim::air_floor_y(const Vehicle& v) const {
  if (!params_.world) return v.pos.y;
  float f = params_.world->terrain_height(v.pos.x, v.pos.z);
  const auto dn = params_.world->raycast({v.pos.x, v.pos.y + 4.f, v.pos.z}, {0.f, -1.f, 0.f}, 220.f);
  if (dn.hit && std::fabs(dn.normal.y) > 0.4f && dn.point.y > f + 1.0f &&
      dn.point.y <= v.pos.y + 4.f) {
    f = dn.point.y;
  }
  return f + v.land_clearance;
}

void GameSim::rebuild_vehicle_model(Vehicle& v) const {
  const float hrad = glm::radians(v.heading);
  if (v.is_air) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), v.pos);
    m = glm::rotate(m, hrad, glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(-v.pitch), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(-v.roll), glm::vec3(0, 0, 1));
    v.model = m;
  } else if (v.is_boat) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), v.pos);
    m = glm::rotate(m, hrad, glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(-v.pitch), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(-v.roll), glm::vec3(0, 0, 1));
    v.model = m;
  } else {
    const glm::vec3 up = v.ground_normal;
    const glm::vec3 fdir(std::sin(hrad), 0.f, std::cos(hrad));
    const glm::vec3 rgt = glm::normalize(glm::cross(up, fdir));
    const glm::vec3 fwd2 = glm::normalize(glm::cross(rgt, up));
    glm::mat4 R(1.0f);
    R[0] = glm::vec4(rgt, 0.f);
    R[1] = glm::vec4(up, 0.f);
    R[2] = glm::vec4(fwd2, 0.f);
    v.model = glm::translate(glm::mat4(1.0f), v.pos) * R;
  }
}

void GameSim::update_capsules(Enemy& en, const bf2::AnimationClip* clip, int frame) const {
  if (!params_.soldier_ske || !clip) return;
  const bf2::PosedSkeleton posed = bf2::pose_skeleton(*params_.soldier_ske, clip, frame);
  if (posed.world_positions.size() < 48) return;
  const float cy = std::cos(en.yaw), sy = std::sin(en.yaw);
  auto to_world = [&](const glm::vec3& m) {
    return glm::vec3(en.pos.x + m.x * cy + m.z * sy, en.pos.y + m.y,
                     en.pos.z - m.x * sy + m.z * cy);
  };
  en.caps.clear();
  for (const auto& bp : kBonePairs) {
    if (bp.a >= static_cast<int>(posed.world_positions.size()) ||
        bp.b >= static_cast<int>(posed.world_positions.size()))
      continue;
    HitCapsule c;
    c.a = to_world(posed.world_positions[bp.a]);
    c.b = to_world(posed.world_positions[bp.b]);
    c.r = bp.r;
    c.zone = bp.zone;
    en.caps.push_back(c);
  }
}

EnemyHit GameSim::shoot_enemies(const glm::vec3& o, const glm::vec3& dir, float maxd) const {
  EnemyHit best;
  const glm::vec3 q1 = o + dir * maxd;
  for (std::size_t i = 0; i < state_.enemies.size(); ++i) {
    if (!state_.enemies[i].alive) continue;
    for (const auto& c : state_.enemies[i].caps) {
      float s;
      const float d2 = closest_seg_seg_sq(o, q1, c.a, c.b, s);
      if (d2 <= c.r * c.r) {
        const float t = s * maxd;
        if (t < best.dist) {
          best.dist = t;
          best.idx = static_cast<int>(i);
          best.zone = c.zone;
          best.point = o + dir * t;
        }
      }
    }
  }
  return best;
}

void GameSim::damage_enemy(int idx, int zone) {
  damage_enemy(idx, zone, -1.f);
}

void GameSim::damage_enemy(int idx, int zone, float weapon_damage_override) {
  if (idx < 0 || idx >= static_cast<int>(state_.enemies.size())) return;
  Enemy& en = state_.enemies[idx];
  const float weapon_dmg = weapon_damage_override > 0.f
                               ? weapon_damage_override
                               : (params_.weapon.valid ? params_.weapon.damage : 28.f);
  const float base = zone == 2 ? weapon_dmg * 1.45f : (zone == 1 ? weapon_dmg : weapon_dmg * 0.65f);
  const HitboxZone hz =
      zone == 2 ? HitboxZone::Head : (zone == 1 ? HitboxZone::Torso : HitboxZone::Limb);
  const float dmg = apply_hitbox_multiplier(hz, base);
  en.health -= dmg;
  en.hit_flash = 0.12f;
  en.alert = 1.f;
  if (en.health <= 0.f) {
    en.alive = false;
    en.death_time = 0.f;
    ++state_.player_kills;
    const std::string victim = en.name.empty() ? "Enemy" : en.name;
    push_kill_feed(params_.player_label.empty() ? "You" : params_.player_label, victim,
                   player_weapon_label());
  }
}

void GameSim::hurt_player(float damage, int killer_enemy_idx, const char* killer_override) {
  if (state_.match_over || damage <= 0.f) return;
  state_.player_health -= damage;
  state_.player_regen_delay = 3.f;
  if (state_.player_health <= 0.f) {
    ++state_.player_deaths;
    apply_death_ticket(state_.tickets, state_.player_team, conquest_cfg_);
    std::string killer = "Unknown";
    if (killer_override && killer_override[0]) {
      killer = killer_override;
    } else if (killer_enemy_idx >= 0 &&
               killer_enemy_idx < static_cast<int>(state_.enemies.size())) {
      const Enemy& ke = state_.enemies[static_cast<std::size_t>(killer_enemy_idx)];
      killer = ke.name.empty() ? "Enemy" : ke.name;
    }
    push_kill_feed(killer, params_.player_label.empty() ? "You" : params_.player_label,
                   (killer_override && std::strcmp(killer_override, "Vehicle") == 0)
                       ? "Vehicle"
                       : enemy_weapon_label());
    state_.player_health = 100.f;
    events_.open_deploy = true;
  }
}

void GameSim::explode_at(const glm::vec3& center, float radius, float max_damage) {
  spawn_missile_detonation_fx(state_.smoke, state_.explosions, center, glm::clamp(radius / 9.f, 0.5f, 2.f));
  for (auto& en : state_.enemies) {
    if (!en.alive) continue;
    const glm::vec3 chest(en.pos.x, en.pos.y + 1.0f, en.pos.z);
    const float d = glm::length(chest - center);
    if (d > radius) continue;
    const float falloff = 1.f - (d / radius);
    en.health -= max_damage * falloff * falloff;
    en.hit_flash = 0.15f;
    en.alert = 1.f;
    if (en.health <= 0.f) {
      en.alive = false;
      en.death_time = 0.f;
      ++state_.player_kills;
      const std::string victim = en.name.empty() ? "Enemy" : en.name;
      push_kill_feed(params_.player_label.empty() ? "You" : params_.player_label, victim,
                     "Explosives");
    }
  }
  const glm::vec3 player_chest(state_.player.position.x, state_.player.position.y + 0.4f,
                               state_.player.position.z);
  const float pd = glm::length(player_chest - center);
  if (pd <= radius) {
    const float falloff = 1.f - (pd / radius);
    float dmg = max_damage * falloff * falloff;
    if (state_.in_vehicle >= 0) dmg *= 0.55f;
    hurt_player(dmg);
  }
}

void GameSim::spawn_missile_from_profile(const glm::vec3& origin, const glm::vec3& dir,
                                         const ProjectileProfile& profile, bool homing_target) {
  ActiveMissile am;
  am.m.position = origin;
  am.m.guided = profile.guided && homing_target;
  am.m.has_target = am.m.guided;
  am.m.velocity = glm::normalize(dir) * std::max(10.f, profile.launch_velocity);
  am.m.boost_accel = std::max(50.f, profile.acceleration);
  am.m.boost_time = profile.guided ? 1.7f : 1.1f;
  am.m.max_speed = std::max(40.f, profile.max_speed);
  am.m.turn_rate = profile.guided ? std::max(0.5f, profile.turn_rate) : 0.f;
  am.m.life = std::max(3.f, profile.life);
  am.m.gravity = 9.81f * std::max(0.f, profile.gravity_modifier);
  am.m.mass = std::max(1.f, profile.mass);
  am.m.drag = std::max(0.0001f, profile.drag * 0.00055f);
  am.explosion_radius = std::max(1.f, profile.explosion_radius);
  am.explosion_damage = std::max(10.f, profile.explosion_damage);
  am.prev_pos = origin;
  state_.missiles.push_back(am);
  spawn_rocket_launch_fx(state_.smoke, origin, dir);
}

void GameSim::fire_vehicle_projectile(const glm::vec3& origin, const glm::vec3& dir,
                                      const VehicleWeaponSlot& weapon_slot) {
  ProjectileProfile p = weapon_slot.projectile;
  if (!p.valid) {
    p.damage = weapon_slot.damage;
    p.launch_velocity = 60.f;
    p.acceleration = 190.f;
    p.max_speed = 240.f;
    p.explosion_radius = 6.f;
    p.explosion_damage = 150.f;
    p.life = 6.f;
    p.valid = true;
  }
  spawn_missile_from_profile(origin, dir, p, false);
}

void GameSim::fire_heli_rocket(const glm::vec3& origin, const glm::vec3& dir,
                               const VehicleWeaponSlot* weapon_slot) {
  if (weapon_slot && weapon_slot->valid) {
    fire_vehicle_projectile(origin, dir, *weapon_slot);
    return;
  }
  VehicleWeaponSlot fallback;
  fallback.valid = true;
  fallback.damage = 150.f;
  fallback.projectile.damage = 150.f;
  fallback.projectile.launch_velocity = 60.f;
  fallback.projectile.acceleration = 190.f;
  fallback.projectile.max_speed = 240.f;
  fallback.projectile.explosion_radius = 6.f;
  fallback.projectile.explosion_damage = 150.f;
  fallback.projectile.life = 6.f;
  fallback.projectile.valid = true;
  fire_vehicle_projectile(origin, dir, fallback);
}

void GameSim::step_vehicle_fatal_collisions() {
  if (!params_.world) return;
  const glm::vec3 player_feet(state_.player.position.x, state_.player.position.y,
                              state_.player.position.z);
  for (std::size_t vi = 0; vi < state_.vehicles.size(); ++vi) {
    const Vehicle& veh = state_.vehicles[vi];
    if (veh.mesh_key.find("vehicles/") == std::string::npos) continue;
    float spd = std::fabs(veh.speed);
    if (veh.is_air) spd = glm::length(glm::vec3(veh.vel.x, 0.f, veh.vel.z));
    if (veh.is_air && !veh.is_heli) spd = glm::length(veh.vel);
    if (spd < 9.f) continue;
    const float kill_r = std::max(veh.col_half.x, veh.col_half.z) + 1.2f;
    const float crush_dmg = 120.f + spd * 4.f;

    for (auto& en : state_.enemies) {
      if (!en.alive) continue;
      const glm::vec2 d(en.pos.x - veh.pos.x, en.pos.z - veh.pos.z);
      if (glm::dot(d, d) > kill_r * kill_r) continue;
      if (std::fabs(en.pos.y - veh.pos.y) > veh.col_half.y + 2.5f) continue;
      en.alive = false;
      en.health = 0.f;
      en.death_time = 0.f;
      ++state_.player_kills;
      const std::string victim = en.name.empty() ? "Enemy" : en.name;
      push_kill_feed(params_.player_label.empty() ? "You" : params_.player_label, victim,
                     "Vehicle");
      spawn_missile_detonation_fx(state_.smoke, state_.explosions,
                                  glm::vec3(en.pos.x, en.pos.y + 1.f, en.pos.z), 0.35f);
    }

    if (state_.in_vehicle < 0) {
      const glm::vec2 d(player_feet.x - veh.pos.x, player_feet.z - veh.pos.z);
      if (glm::dot(d, d) <= kill_r * kill_r &&
          std::fabs(player_feet.y - veh.pos.y) <= veh.col_half.y + 2.5f) {
        hurt_player(crush_dmg, -1, "Vehicle");
      }
    }
  }
}

bool GameSim::push_out_of_vehicles(float& x, float& z, float feet_y, int ignore) const {
  bool moved = false;
  for (std::size_t vi = 0; vi < state_.vehicles.size(); ++vi) {
    if (static_cast<int>(vi) == ignore) continue;
    const Vehicle& v = state_.vehicles[vi];
    if (v.mesh_key.find("vehicles/") == std::string::npos) continue;
    const float base = v.pos.y - v.clearance;
    if (feet_y > base + v.col_half.y * 2.f + 0.3f || feet_y < base - 2.0f) continue;
    const float hd = glm::radians(v.heading);
    const glm::vec3 fwd(std::sin(hd), 0.f, std::cos(hd));
    const glm::vec3 rgt(fwd.z, 0.f, -fwd.x);
    const float dx = x - v.pos.x, dz = z - v.pos.z;
    float lr = dx * rgt.x + dz * rgt.z;
    float lf = dx * fwd.x + dz * fwd.z;
    const float pr = 0.4f;
    const float ex = v.col_half.x + pr, ez = v.col_half.z + pr;
    if (std::fabs(lr) >= ex || std::fabs(lf) >= ez) continue;
    const float penR = ex - std::fabs(lr);
    const float penF = ez - std::fabs(lf);
    if (penR < penF) {
      lr += (lr >= 0.f ? penR : -penR);
    } else {
      lf += (lf >= 0.f ? penF : -penF);
    }
    x = v.pos.x + rgt.x * lr + fwd.x * lf;
    z = v.pos.z + rgt.z * lr + fwd.z * lf;
    moved = true;
  }
  return moved;
}

bool GameSim::is_trapped_spawn(float x, float z, float refy, float& out_y) const {
  const float feet = ground_surface(x, z, refy);
  out_y = feet;
  if (feet < params_.water_y - 0.3f) return true;
  if (!params_.world) return false;
  const auto up = params_.world->raycast({x, feet + 0.2f, z}, {0.f, 1.f, 0.f}, 3.0f);
  const bool roofed = up.hit && up.distance < 2.6f;
  int walls = 0;
  const float chest = feet + 1.0f;
  for (int a = 0; a < 8; ++a) {
    const float ang = a * 0.7853981f;
    const glm::vec3 d(std::cos(ang), 0.f, std::sin(ang));
    const auto h = params_.world->raycast({x, chest, z}, {d.x, d.y, d.z}, 1.2f);
    if (h.hit && std::fabs(h.normal.y) < 0.6f) ++walls;
  }
  return roofed && walls >= 5;
}

glm::vec3 GameSim::find_safe_spawn(const glm::vec3& desired) const {
  const float refy = desired.y;
  float x = desired.x, z = desired.z, y = 0.f;
  push_out_of_vehicles(x, z, ground_surface(x, z, refy), -1);
  if (!is_trapped_spawn(x, z, refy, y)) return {x, ground_surface(x, z, refy), z};
  for (float r = 3.f; r <= 40.f; r += 3.f) {
    for (int a = 0; a < 12; ++a) {
      const float ang = a * 0.5235987f;
      float cx = desired.x + std::cos(ang) * r;
      float cz = desired.z + std::sin(ang) * r;
      push_out_of_vehicles(cx, cz, ground_surface(cx, cz, refy), -1);
      if (!is_trapped_spawn(cx, cz, refy, y)) return {cx, ground_surface(cx, cz, refy), cz};
    }
  }
  float fy = ground_surface(x, z, refy);
  if (fy < params_.water_y) fy = params_.water_y;
  return {x, fy, z};
}

void GameSim::step_vehicle_interaction(const PlayerInput& input) {
#include "game_sim_interaction.inl"
}

void GameSim::step_push_player_from_hulls() {
  if (state_.in_vehicle >= 0) return;
  float px = state_.player.position.x, pz = state_.player.position.z;
  if (push_out_of_vehicles(px, pz, state_.player.position.y - state_.player.eye_height,
                           state_.in_vehicle)) {
    state_.player.position.x = px;
    state_.player.position.z = pz;
  }
}

void GameSim::decay_sticks(float dt, const PlayerInput& input) {
  state_.air_input_grace = std::max(0.f, state_.air_input_grace - dt);
  if (state_.in_vehicle >= 0 && state_.in_vehicle < static_cast<int>(state_.vehicles.size()) &&
      state_.player_seat == 0 && state_.air_input_grace <= 0.f && !input.air_stick_moved) {
    const Vehicle& av = state_.vehicles[state_.in_vehicle];
    if (!av.is_heli) {
      const float decay = std::exp(-5.f * dt);
      state_.air_pitch_stick *= decay;
      state_.air_roll_stick *= decay;
    }
  }
  float pitch_stick = input.air_pitch_stick;
  if (input.pitch_up) pitch_stick = std::max(pitch_stick, 1.f);
  state_.air_pitch_stick = pitch_stick;
  state_.air_roll_stick = input.air_roll_stick;
}

void GameSim::step_rotor_spool(float dt, const PlayerInput& input) {
  const float fdt = dt > 0.f ? dt : 1.f / 60.f;
  for (std::size_t i = 0; i < state_.vehicles.size(); ++i) {
    Vehicle& av = state_.vehicles[i];
    if (!av.is_air) continue;
    const bool occupied = static_cast<int>(i) == state_.in_vehicle;
    float target = occupied ? 1.f : 0.f;
    float rate = target > av.rotor_rpm ? 1.f / av.rotor_spool_up : 1.f / av.rotor_spool_down;
    if (occupied && av.is_heli && (input.throttle_up || input.throttle_down || input.pitch_up)) {
      rate = 1.f / av.rotor_spool_collective;
    }
    av.rotor_rpm =
        glm::clamp(av.rotor_rpm + glm::clamp(target - av.rotor_rpm, -rate * fdt, rate * fdt), 0.f, 1.f);
    av.rotor_spin += fdt * av.rotor_spin_rate * av.rotor_rpm;
  }
  for (auto& gv : state_.vehicles) {
    if (gv.wheels.empty()) continue;
    float rate = gv.speed / 0.32f;
    if (gv.is_air && !gv.is_heli) {
      rate = glm::length(glm::vec2(gv.vel.x, gv.vel.z)) / 0.32f;
    } else if (gv.is_air) {
      continue;
    }
    for (float& ws : gv.wheel_spin) ws += rate * fdt;
  }
}

void GameSim::step_vehicles(float dt, const PlayerInput& input) {
#include "game_sim_vehicles.inl"
}

void GameSim::step_player_on_foot(float dt, const PlayerInput& input) {
  if (!params_.world || state_.in_vehicle >= 0 || input.drone_mode) return;
  if (input.deploy_open) {
    state_.player.desired_velocity = {0.f, 0.f, 0.f};
    params_.world->snap_character_to_ground(state_.player);
    return;
  }
  if (input.prone_toggle) {
    state_.infantry_pose =
        state_.infantry_pose == SoldierPose::Prone ? SoldierPose::Stand : SoldierPose::Prone;
  } else if (input.crouch && state_.infantry_pose != SoldierPose::Prone) {
    state_.infantry_pose = SoldierPose::Crouch;
  } else if (state_.infantry_pose != SoldierPose::Prone) {
    state_.infantry_pose = SoldierPose::Stand;
  }

  if (state_.infantry_pose == SoldierPose::Prone) {
    state_.player.eye_height = 0.55f;
  } else if (state_.infantry_pose == SoldierPose::Crouch) {
    state_.player.eye_height = 1.25f;
  } else {
    state_.player.eye_height = 1.8f;
  }

  glm::vec3 move = input.move_wish;
  const bool wants_move = glm::length(move) > 0.001f;
  const bool can_sprint = input.sprint && wants_move && state_.player_stamina > 5.f &&
                          state_.infantry_pose == SoldierPose::Stand;
  if (can_sprint) {
    state_.player_stamina = std::max(0.f, state_.player_stamina - 22.f * dt);
  } else {
    state_.player_stamina =
        std::min(100.f, state_.player_stamina + (wants_move ? 8.f : 16.f) * dt);
  }
  float speed = 6.5f;
  if (state_.infantry_pose == SoldierPose::Crouch) {
    speed = 4.f;
  } else if (state_.infantry_pose == SoldierPose::Prone) {
    speed = 1.8f;
  } else if (can_sprint) {
    speed = 12.f;
  }
  if (wants_move) move = glm::normalize(move) * speed;
  state_.player.desired_velocity = {move.x, 0.f, move.z};
  if (input.jump && state_.player.on_ground && state_.infantry_pose == SoldierPose::Stand) {
    state_.player.vertical_velocity = 6.5f;
  }
  params_.world->step_character(state_.player, dt > 0.f ? dt : 1.f / 60.f);
}

void GameSim::set_weapon_profile(const WeaponProfile& weapon, bool refill_ammo) {
  params_.weapon = weapon;
  if (!refill_ammo) return;
  const int mag = weapon.magazine_size > 0 ? weapon.magazine_size : kMagSize;
  state_.mag_ammo = mag;
  state_.reserve_ammo = weapon.reserve_ammo > 0 ? weapon.reserve_ammo : mag * 5;
  state_.reloading = false;
  state_.reload_timer = 0.f;
  state_.fire_deviation = weapon.spread;
  state_.shots_fired = 0;
  state_.burst_shots_left = 0;
  state_.burst_pause_timer = 0.f;
}

void GameSim::set_at_missile_profile(const ProjectileProfile& profile) {
  params_.at_missile = profile;
}

void GameSim::step_combat(float dt, const PlayerInput& input) {
  state_.fire_cooldown = std::max(0.f, state_.fire_cooldown - dt);
  state_.muzzle_flash = std::max(0.f, state_.muzzle_flash - dt);
  state_.recoil = std::max(0.f, state_.recoil - dt * 6.f);
  state_.fire_deviation = std::max(params_.weapon.spread,
                                   state_.fire_deviation - dt * params_.weapon.deviation_decay);
  state_.reload_timer = std::max(0.f, state_.reload_timer - dt);
  state_.burst_pause_timer = std::max(0.f, state_.burst_pause_timer - dt);
  const bool fire_edge = input.fire && !state_.fire_was_down;
  state_.fire_was_down = input.fire;
  const int mag_cap =
      params_.weapon.magazine_size > 0 ? params_.weapon.magazine_size : kMagSize;
  if (state_.reloading && state_.reload_timer <= 0.f) {
    const int need = mag_cap - state_.mag_ammo;
    const int take = std::min(need, state_.reserve_ammo);
    state_.mag_ammo += take;
    state_.reserve_ammo -= take;
    state_.reloading = false;
    events_.play_reload = true;
  }

  const int burst_sz = std::max(1, params_.weapon.burst_size);
  const bool burst_weapon = burst_sz > 1;
  if (burst_weapon && fire_edge && state_.burst_shots_left <= 0 && state_.burst_pause_timer <= 0.f &&
      !state_.reloading && state_.mag_ammo > 0) {
    state_.burst_shots_left = burst_sz;
  }

  if (input.fire && input.mouse_look && !input.drone_mode && state_.in_vehicle < 0 &&
      !input.deploy_open && !state_.reloading && state_.mag_ammo <= 0) {
    events_.out_of_ammo_voice = true;
  }

  const bool want_shoot =
      input.mouse_look && !input.drone_mode && state_.in_vehicle < 0 && !input.deploy_open &&
      !state_.reloading && state_.mag_ammo > 0 && state_.fire_cooldown <= 0.f &&
      (burst_weapon ? state_.burst_shots_left > 0 : input.fire);

  if (want_shoot) {
    --state_.mag_ammo;
    if (burst_weapon) {
      --state_.burst_shots_left;
      if (state_.burst_shots_left <= 0) state_.burst_pause_timer = params_.weapon.burst_pause;
    }
    if (state_.mag_ammo == 0 && state_.reserve_ammo > 0) {
      state_.reloading = true;
      state_.reload_timer =
          params_.weapon.reload_time > 0.f ? params_.weapon.reload_time : 2.0f;
      events_.play_reload = true;
    }
    const float shot_rate = burst_weapon && params_.weapon.burst_shot_rate > 0.f
                                ? params_.weapon.burst_shot_rate
                                : (params_.weapon.valid ? params_.weapon.fire_rate : 0.1f);
    state_.fire_cooldown = shot_rate;
    state_.muzzle_flash = 0.045f;
    state_.recoil = std::min(1.f, state_.recoil + 0.6f);
    events_.fired_shot = true;
    ++state_.shots_fired;
    const float dev = glm::clamp(state_.fire_deviation + state_.recoil * 0.05f,
                                 params_.weapon.min_deviation, params_.weapon.max_deviation);
    state_.fire_deviation =
        std::min(params_.weapon.max_deviation, dev + params_.weapon.spread_per_shot);
    glm::vec3 front = input.look_forward;
    if (dev > 1e-5f) {
      const glm::vec3 up = glm::normalize(glm::cross(input.look_right, front));
      front = glm::normalize(front + input.look_right * ((frand() - 0.5f) * 2.f * dev) +
                             up * ((frand() - 0.5f) * 2.f * dev));
    }
    const glm::vec3& right = input.look_right;
    const glm::vec3 muzzle =
        input.eye + front * 0.6f + right * 0.18f - glm::vec3(0, 1, 0) * 0.12f;
    events_.fire_origin = muzzle;
    events_.fire_dir = front;
    spawn_muzzle_smoke_fx(state_.smoke, muzzle, front);
    const bool emit_tracer =
        params_.weapon.tracer_count <= 0 ||
        (state_.shots_fired % std::max(1, params_.weapon.tracer_count) == 0);
    if (state_.ballistic) {
      const float spd = params_.weapon.tracer_speed > 1.f ? params_.weapon.tracer_speed : kMuzzleSpeed;
      state_.projectiles.push_back({muzzle, front * spd, kBulletLife});
    } else if (params_.world) {
      const auto hit = params_.world->raycast({input.eye.x, input.eye.y, input.eye.z},
                                              {front.x, front.y, front.z}, 400.f);
      const float terr = hit.hit ? hit.distance : 400.f;
      const auto eh = shoot_enemies(input.eye, front, terr);
      if (eh.idx >= 0) {
        damage_enemy(eh.idx, eh.zone);
        if (emit_tracer) state_.tracers.push_back({muzzle, eh.point, 0.06f});
        state_.impacts.push_back({eh.point, 0.4f});
        spawn_bullet_impact_fx(state_.smoke, eh.point, -front, "grass");
      } else {
        const glm::vec3 end =
            hit.hit ? glm::vec3(hit.point.x, hit.point.y, hit.point.z) : input.eye + front * 400.f;
        if (emit_tracer) state_.tracers.push_back({muzzle, end, 0.06f});
        if (hit.hit) {
          state_.impacts.push_back({end, 0.6f});
          spawn_bullet_impact_fx(state_.smoke, end, -front, "grass");
        }
      }
    }
  }
}

void GameSim::step_projectiles(float dt) {
  if (!params_.world) return;
  const float gust = 0.7f + 0.3f * std::sin(dt * 37.f);  // cheap variation
  state_.wind = state_.wind_base * gust;
  for (auto& pr : state_.projectiles) {
    pr.life -= dt;
    if (pr.life <= 0.f) continue;
    pr.vel.y += kBulletGravity * dt;
    const glm::vec3 rel = pr.vel - state_.wind;
    const float rspd = glm::length(rel);
    if (rspd > 1e-3f) pr.vel -= rel * (kBulletDrag * rspd * dt);
    const glm::vec3 next = pr.pos + pr.vel * dt;
    const glm::vec3 seg = next - pr.pos;
    const float seg_len = glm::length(seg);
    if (seg_len > 1e-5f) {
      const glm::vec3 sd = seg / seg_len;
      const auto hit =
          params_.world->raycast({pr.pos.x, pr.pos.y, pr.pos.z}, {seg.x, seg.y, seg.z}, seg_len);
      const float terr = hit.hit ? hit.distance : 1e30f;
      const auto eh = shoot_enemies(pr.pos, sd, seg_len);
      if (eh.idx >= 0 && eh.dist < terr) {
        damage_enemy(eh.idx, eh.zone);
        state_.impacts.push_back({eh.point, 0.4f});
        spawn_bullet_impact_fx(state_.smoke, eh.point, -sd, "grass");
        state_.tracers.push_back({pr.pos, eh.point, 0.05f});
        pr.life = 0.f;
        continue;
      }
      if (hit.hit) {
        const glm::vec3 hp(hit.point.x, hit.point.y, hit.point.z);
        state_.impacts.push_back({hp, 0.6f});
        spawn_bullet_impact_fx(state_.smoke, hp, -sd, "grass");
        state_.tracers.push_back({pr.pos, hp, 0.05f});
        pr.life = 0.f;
        continue;
      }
    }
    pr.pos = next;
  }
}

void GameSim::step_enemies(float dt, const PlayerInput& input) {
  if (!params_.world || state_.match_over) return;
  if (!params_.bots_enabled || params_.bot_count <= 0) return;
  const glm::vec3 eye(state_.player.position.x, state_.player.position.y, state_.player.position.z);
  state_.player_regen_delay = std::max(0.f, state_.player_regen_delay - dt);
  if (state_.player_regen_delay <= 0.f && state_.player_health < 100.f) {
    state_.player_health = std::min(100.f, state_.player_health + 12.f * dt);
  }
  const int diff = std::clamp(params_.bot_difficulty, 1, 5);
  const float diff_t = static_cast<float>(diff - 1) / 4.f;
  const float kSightRange = 45.f + diff_t * 35.f;
  const float kEngageRange = 32.f + diff_t * 18.f;
  const int kMaxShooters = 1 + static_cast<int>(diff_t * 3.f);
  const float alert_gain = 0.35f + diff_t * 0.85f;
  const float spread_scale = 1.8f - diff_t * 1.2f;
  struct Contact {
    int idx;
    float dist;
  };
  std::vector<Contact> contacts;
  for (std::size_t i = 0; i < state_.enemies.size(); ++i) {
    Enemy& en = state_.enemies[i];
    if (!en.alive) {
      en.death_time += dt;
      if (en.death_time > 330.f) {
        en.pos = en.home;
        snap_enemy_feet(en);
        en.alive = true;
        en.health = 100.f;
        en.hit_flash = 0.f;
        en.alert = 0.f;
        en.burst_left = 0;
        en.burst_cooldown = 1.f + frand();
      }
      continue;
    }
    en.hit_flash = std::max(0.f, en.hit_flash - dt);
    const bool hostile_to_player =
        en.team != state_.player_team && en.team != TeamId::Neutral;
    float move_speed = 0.f;
    const auto try_step = [&](const glm::vec3& step) {
      if (!params_.world) {
        en.pos.x += step.x;
        en.pos.z += step.z;
        return;
      }
      const glm::vec3 chest = en.pos + glm::vec3(0.f, 1.2f, 0.f);
      const float len = glm::length(glm::vec2(step.x, step.z));
      if (len < 1e-4f) return;
      const glm::vec3 dir(step.x / len, 0.f, step.z / len);
      const auto hit = params_.world->raycast({chest.x, chest.y, chest.z}, {dir.x, dir.y, dir.z},
                                              len + 0.35f);
      if (hit.hit && std::fabs(hit.normal.y) < 0.45f && hit.distance < len + 0.1f) {
        const glm::vec3 slide = glm::vec3(hit.normal.x, 0.f, hit.normal.z);
        if (glm::length(slide) > 0.01f) {
          const glm::vec3 sdir = glm::normalize(slide);
          en.pos.x += sdir.x * len * 0.35f;
          en.pos.z += sdir.z * len * 0.35f;
        }
        return;
      }
      en.pos.x += step.x;
      en.pos.z += step.z;
    };
    en.anim_time += dt;
    const glm::vec3 chest = en.pos + glm::vec3(0.f, 1.35f, 0.f);
    const glm::vec3 to_player = eye - chest;
    const float dist = glm::length(to_player);
    bool los = false;
    if (dist > 1e-3f && dist < kSightRange) {
      const glm::vec3 dir = to_player / dist;
      const auto hit = params_.world->raycast({chest.x, chest.y, chest.z}, {dir.x, dir.y, dir.z}, dist);
      los = !hit.hit || hit.distance >= dist - 1.5f;
    }
    en.alert = std::clamp(en.alert + (hostile_to_player && los ? dt * alert_gain / 1.6f : -dt / 2.5f),
                          0.f, 1.f);
    en.moving = false;
    if (hostile_to_player && los && dist < kEngageRange) {
      en.yaw = std::atan2(to_player.x, to_player.z);
      if (dist > 35.f) {
        glm::vec3 goal = glm::vec3(eye.x, en.pos.y, eye.z);
        if (params_.nav_mesh && params_.nav_mesh->valid()) {
          goal = params_.nav_mesh->waypoint_along_path(en.pos, goal, 3.0f * dt);
        } else {
          goal = en.pos + glm::normalize(glm::vec3(to_player.x, 0.f, to_player.z)) * 3.0f * dt;
        }
        const glm::vec3 step = goal - en.pos;
        try_step(step);
        move_speed = 3.0f;
        en.moving = true;
      }
      contacts.push_back({static_cast<int>(i), dist});
    } else if (hostile_to_player && los) {
      en.yaw = std::atan2(to_player.x, to_player.z);
      glm::vec3 goal = glm::vec3(eye.x, en.pos.y, eye.z);
      if (params_.nav_mesh && params_.nav_mesh->valid()) {
        goal = params_.nav_mesh->waypoint_along_path(en.pos, goal, 2.4f * dt);
      } else {
        goal = en.pos + glm::normalize(glm::vec3(to_player.x, 0.f, to_player.z)) * 2.4f * dt;
      }
      const glm::vec3 step = goal - en.pos;
      try_step(step);
      move_speed = 2.4f;
      en.moving = true;
      } else {
        glm::vec3 goal = en.patrol_target;
        if (params_.nav_mesh && params_.nav_mesh->valid()) {
          goal = params_.nav_mesh->waypoint_along_path(en.pos, goal, 1.5f * dt);
        }
        const glm::vec2 to_wp(goal.x - en.pos.x, goal.z - en.pos.z);
      const float wp_dist = glm::length(to_wp);
      if (wp_dist < 1.2f) {
        en.patrol_wait -= dt;
        if (en.patrol_wait <= 0.f) {
          const float a = frand() * 6.2831853f;
          const float r = 3.f + frand() * 13.f;
          en.patrol_target =
              glm::vec3(en.home.x + std::cos(a) * r, 0.f, en.home.z + std::sin(a) * r);
          en.patrol_wait = 1.5f + frand() * 3.5f;
        }
      } else {
        const glm::vec3 step = glm::normalize(glm::vec3(to_wp.x, 0.f, to_wp.y)) * 1.5f * dt;
        try_step(step);
        move_speed = 1.5f;
        en.yaw = std::atan2(to_wp.x, to_wp.y);
        en.moving = true;
      }
    }
    snap_enemy_feet(en);
    const bf2::AnimationClip* clip = nullptr;
    float anim_rate = 1.f;
    if (en.moving) {
      if (move_speed > 2.2f && params_.have_clip_run)
        clip = params_.clip_run;
      else if (params_.have_clip_walk)
        clip = params_.clip_walk;
      const float ref = (clip == params_.clip_run) ? 5.5f : 1.5f;
      anim_rate = glm::clamp(move_speed / ref, 0.6f, 1.8f);
    } else if (params_.have_clip_stand) {
      clip = params_.clip_stand;
    }
    en.anim_time += dt * anim_rate;
    int frame = 0;
    if (clip && clip->frame_count > 0)
      frame = static_cast<int>(en.anim_time * 30.f) % clip->frame_count;
    update_capsules(en, clip, frame);
  }
  std::sort(contacts.begin(), contacts.end(),
            [](const Contact& a, const Contact& b) { return a.dist < b.dist; });
  for (std::size_t c = 0; c < contacts.size(); ++c) {
    Enemy& en = state_.enemies[contacts[c].idx];
    const float dist = contacts[c].dist;
    const bool can_shoot = static_cast<int>(c) < kMaxShooters && en.alert >= 1.0f;
    en.burst_cooldown = std::max(0.f, en.burst_cooldown - dt);
    if (!can_shoot) continue;
    if (en.burst_left <= 0) {
      if (en.burst_cooldown <= 0.f) {
        en.burst_left = 3 + (std::rand() % 3);
        en.shot_timer = 0.f;
      }
      continue;
    }
    en.shot_timer -= dt;
    if (en.shot_timer > 0.f) continue;
    en.shot_timer = std::max(0.05f, en.fire_rate);
    if (--en.burst_left <= 0) en.burst_cooldown = 2.4f + frand() * 2.4f;
    const glm::vec3 chest = en.pos + glm::vec3(0.f, 1.35f, 0.f);
    const glm::vec3 dir = glm::normalize(eye - chest);
    const glm::vec3 muz = chest + dir * 0.3f;
    const float sp = (en.spread + dist * 0.0015f) * spread_scale;
    const glm::vec3 aim =
        eye + glm::vec3((frand() - 0.5f), (frand() - 0.5f), (frand() - 0.5f)) * sp;
    const glm::vec3 shot_dir = glm::normalize(aim - muz);
    state_.tracers.push_back({muz, aim, 0.08f});
    spawn_muzzle_smoke_fx(state_.smoke, muz, shot_dir);
    bool hit_player = false;
    if (!input.deploy_open && state_.in_vehicle < 0) {
      const glm::vec3 to_eye = eye - muz;
      const float along = glm::dot(to_eye, shot_dir);
      if (along > 0.5f && along < dist + 1.5f) {
        const glm::vec3 closest = muz + shot_dir * along;
        if (glm::length(closest - eye) < 0.55f) hit_player = true;
      }
      if (!hit_player && params_.world) {
        const auto los = params_.world->raycast({muz.x, muz.y, muz.z},
                                                {shot_dir.x, shot_dir.y, shot_dir.z}, dist + 2.f);
        if (!los.hit || los.distance >= dist - 0.8f) {
          const glm::vec3 to_eye2 = eye - muz;
          const float along2 = glm::dot(to_eye2, shot_dir);
          if (along2 > 0.f && along2 < dist + 1.f) {
            const glm::vec3 closest2 = muz + shot_dir * along2;
            if (glm::length(closest2 - eye) < 0.65f) hit_player = true;
          }
        }
      }
    }
    if (hit_player) {
      const float base_dmg = en.damage > 1.f ? en.damage : 12.f;
      hurt_player(base_dmg * (0.75f + frand() * 0.35f), contacts[c].idx);
    }
  }
}

void GameSim::decay_effects(float dt) {
  for (auto& t : state_.tracers) t.life -= dt;
  for (auto& im : state_.impacts) im.life -= dt;
  state_.tracers.erase(std::remove_if(state_.tracers.begin(), state_.tracers.end(),
                                      [](const Tracer& t) { return t.life <= 0.f; }),
                       state_.tracers.end());
  state_.impacts.erase(std::remove_if(state_.impacts.begin(), state_.impacts.end(),
                                      [](const Impact& im) { return im.life <= 0.f; }),
                       state_.impacts.end());
  state_.projectiles.erase(std::remove_if(state_.projectiles.begin(), state_.projectiles.end(),
                                          [](const Projectile& p) { return p.life <= 0.f; }),
                           state_.projectiles.end());
  for (auto& fl : state_.flares) {
    fl.life -= dt;
    fl.v.y -= 6.f * dt;
    const glm::vec3 np = fl.p + fl.v * dt;
    state_.tracers.push_back({fl.p, np, 0.05f});
    fl.p = np;
  }
  state_.flares.erase(std::remove_if(state_.flares.begin(), state_.flares.end(),
                                     [](const Flare& f) { return f.life <= 0.f; }),
                      state_.flares.end());
  for (auto& s : state_.smoke) {
    s.age += dt;
    s.p += s.vel * dt;
    s.vel.y -= 1.8f * dt;
    s.vel *= 1.f - 0.8f * dt;
    if (s.use_graphs) {
      const float t = s.life > 0.f ? glm::clamp(s.age / s.life, 0.f, 1.f) : 1.f;
      s.size = s.birth_size * std::max(0.05f, s.size_graph.eval(t));
    } else {
      s.size += dt * (s.kind == 2 ? 1.6f : 0.45f);
    }
  }
  for (auto& ex : state_.explosions) ex.age += dt;
  state_.smoke.erase(std::remove_if(state_.smoke.begin(), state_.smoke.end(),
                                    [](const Smoke& s) { return s.age >= s.life; }),
                     state_.smoke.end());
  state_.explosions.erase(
      std::remove_if(state_.explosions.begin(), state_.explosions.end(),
                     [](const Explosion& ex) { return ex.age >= ex.life; }),
      state_.explosions.end());
}

void GameSim::step_heli_weapons(float dt, const PlayerInput& input) {
#include "game_sim_heli_weapons.inl"
}

void GameSim::step_missiles(float dt, const PlayerInput& input) {
#include "game_sim_missiles.inl"
}

EnemyHit GameSim::raycast_enemies(const glm::vec3& o, const glm::vec3& dir, float maxd) const {
  return shoot_enemies(o, dir, maxd);
}

void GameSim::apply_explosion(const glm::vec3& center, float radius, float max_damage) {
  explode_at(center, radius, max_damage);
}

void GameSim::apply_enemy_damage(int idx, int zone) { damage_enemy(idx, zone); }

bool GameSim::can_team_spawn_at(const glm::vec3& pos, TeamId team, float epsilon) const {
  if (state_.control_points.empty()) return true;
  const float eps2 = epsilon * epsilon;
  for (const auto& cp : state_.control_points) {
    if (xz_distance_sq(cp.pos, pos) <= eps2) return cp.owner == team;
  }
  for (const auto& cp : state_.control_points) {
    if (xz_distance_sq(cp.pos, pos) <= cp.radius * cp.radius) return cp.owner == team;
  }
  return false;
}

bool GameSim::can_team_spawn_at_cp(int bf2_cp_id, TeamId team) const {
  if (bf2_cp_id == 0) return false;
  for (const auto& cp : state_.control_points) {
    if (cp.bf2_cp_id == bf2_cp_id) return cp.owner == team;
  }
  return false;
}

snapshot::GameState GameSim::build_snapshot(const std::uint32_t local_player_id,
                                            const float player_yaw_deg) const {
  return snapshot::build_snapshot(state_, local_player_id, player_yaw_deg);
}

void GameSim::tick_fixed(float dt, const PlayerInput& input) {
  if (dt <= 0.f) dt = kFixedDt;
  decay_sticks(dt, input);
  step_vehicle_interaction(input);
  step_rotor_spool(dt, input);
  step_vehicles(dt, input);
  step_vehicle_fatal_collisions();
  if (state_.in_vehicle < 0) {
    step_player_on_foot(dt, input);
    step_push_player_from_hulls();
  }
  step_combat(dt, input);
  step_heli_weapons(dt, input);
  step_missiles(dt, input);
  step_projectiles(dt);
  step_enemies(dt, input);
  step_conquest(dt);
  decay_effects(dt);
}

void GameSim::tick(float frame_dt, const PlayerInput& input) {
  if (frame_dt <= 0.f) frame_dt = kFixedDt;
  clear_events();
  time_accumulator_ += frame_dt;
  time_accumulator_ = std::min(time_accumulator_, kFixedDt * 5.f);
  bool first_substep = true;
  while (time_accumulator_ >= kFixedDt) {
    PlayerInput sub = input;
    if (!first_substep) {
      sub.enter_exit = false;
      sub.seat_switch = -1;
      sub.launch_missile = false;
      sub.flare_request = false;
    }
    tick_fixed(kFixedDt, sub);
    first_substep = false;
    time_accumulator_ -= kFixedDt;
  }
}

}  // namespace dalian
