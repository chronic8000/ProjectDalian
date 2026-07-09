state_.heli_rocket_cd = std::max(0.f, state_.heli_rocket_cd - dt);

state_.heli_gun_cd = std::max(0.f, state_.heli_gun_cd - dt);

state_.heli_grocket_cd = std::max(0.f, state_.heli_grocket_cd - dt);

state_.heli_flare_cd = std::max(0.f, state_.heli_flare_cd - dt);



if (state_.in_vehicle < 0 || state_.in_vehicle >= static_cast<int>(state_.vehicles.size())) {

  state_.gunner_target = -1;

  state_.gunner_acquire = 0.f;

  state_.gunner_engaging = false;

  return;

}



Vehicle& hv = state_.vehicles[state_.in_vehicle];

const VehicleWeaponSlot& pilot_rockets =

    hv.weapons.pilot_rockets.valid ? hv.weapons.pilot_rockets : hv.weapons.gunner_rockets;

const VehicleWeaponSlot& gun_slot = hv.weapons.gunner_gun;

const VehicleWeaponSlot& gunner_rockets = hv.weapons.gunner_rockets;



if (!hv.is_air) {

#include "game_sim_tank_weapons.inl"

  return;

}



const float hrad = glm::radians(hv.heading);

const float prad = glm::radians(hv.pitch);

const glm::vec3 nose = glm::normalize(glm::vec3(std::sin(hrad) * std::cos(prad), std::sin(prad),

                                                std::cos(hrad) * std::cos(prad)));

const glm::vec3 rightv(std::cos(hrad), 0.f, -std::sin(hrad));



const bool is_pilot = state_.player_seat == 0;

const float pilot_rocket_cd = pilot_rockets.valid ? pilot_rockets.fire_rate : 0.28f;

if (is_pilot && input.fire && input.mouse_look && !input.deploy_open &&

    state_.heli_rocket_cd <= 0.f && pilot_rockets.valid) {

  state_.heli_rocket_cd = pilot_rocket_cd;

  const glm::vec3 pod = hv.pos + nose * 3.0f - glm::vec3(0.f, 0.4f, 0.f) +

                        rightv * (heli_pod_side_++ % 2 == 0 ? 1.1f : -1.1f);

  fire_heli_rocket(pod, nose, &pilot_rockets);

}



if (input.flare_request && state_.heli_flare_cd <= 0.f) {

  state_.heli_flare_cd = 3.0f;

  for (int i = 0; i < 10; ++i) {

    const float a = (static_cast<float>(i) / 10.f) * 6.2832f;

    const glm::vec3 vv(std::cos(a) * 6.f, -2.f - (i % 3) * 2.f, std::sin(a) * 6.f);

    state_.flares.push_back({hv.pos - glm::vec3(0.f, 0.6f, 0.f), vv, 2.4f});

  }

  for (auto& am : state_.missiles) {

    if (am.m.alive && am.m.guided && glm::distance(am.m.position, hv.pos) < 140.f) {

      am.m.guided = false;

      am.homing_enemy = -1;

    }

  }

}



const int gunner_seat = 1;

const bool has_gunner = hv.seats.size() > static_cast<std::size_t>(gunner_seat);

const bool player_gunning = has_gunner && state_.player_seat == gunner_seat;

const bool ai_gunning =

    has_gunner && hv.seats[gunner_seat].occupant == 1 && state_.player_seat != gunner_seat;

state_.gunner_engaging = false;

if (has_gunner && (ai_gunning || player_gunning)) {

  const glm::vec3 gun = hv.pos + glm::vec3(0.f, -0.6f, 0.f) + nose * 2.0f;

  const glm::vec3 nose_flat = glm::normalize(glm::vec3(nose.x, 0.f, nose.z));

  const float gun_range = gun_slot.valid ? gun_slot.range : 260.f;

  const float gun_cd = gun_slot.valid ? gun_slot.fire_rate : 0.09f;

  const float gun_spread = gun_slot.valid ? gun_slot.min_deviation : 0.05f;



  auto visible = [&](int i, float& out_range) -> bool {

    if (i < 0 || i >= static_cast<int>(state_.enemies.size()) || !state_.enemies[i].alive)

      return false;

    const glm::vec3 ep(state_.enemies[i].pos.x, state_.enemies[i].pos.y + 1.0f,

                       state_.enemies[i].pos.z);

    const glm::vec3 to = ep - gun;

    const float d = glm::length(to);

    if (d < 1e-3f || d > gun_range) return false;

    const glm::vec3 dir = to / d;

    const glm::vec3 dir_flat = glm::normalize(glm::vec3(dir.x, 0.f, dir.z));

    if (glm::dot(dir_flat, nose_flat) < 0.30f) return false;

    if (!params_.world) return false;

    const auto h = params_.world->raycast({gun.x, gun.y, gun.z}, {dir.x, dir.y, dir.z}, d - 1.5f);

    if (h.hit) return false;

    out_range = d;

    return true;

  };



  int tgt = -1;

  float range = 0.f;

  if (ai_gunning) {

    float cur_range = 0.f;

    if (state_.gunner_target >= 0 && visible(state_.gunner_target, cur_range)) {

      tgt = state_.gunner_target;

      range = cur_range;

    } else {

      float best = 1e30f;

      for (std::size_t i = 0; i < state_.enemies.size(); ++i) {

        float r = 0.f;

        if (visible(static_cast<int>(i), r) && r < best) {

          best = r;

          tgt = static_cast<int>(i);

          range = r;

        }

      }

    }

    if (tgt >= 0 && tgt == state_.gunner_target) {

      state_.gunner_acquire += dt;

    } else {

      state_.gunner_acquire = 0.f;

    }

    state_.gunner_target = tgt;

  }



  const bool ai_ready = ai_gunning && tgt >= 0 && state_.gunner_acquire > 0.4f;

  if (player_gunning) {

    const glm::vec3 aim = input.look_forward;

    if (input.fire && input.mouse_look && !input.deploy_open && state_.heli_gun_cd <= 0.f &&

        gun_slot.valid) {

      state_.heli_gun_cd = gun_cd;

      state_.gunner_engaging = true;

      const float sp = gun_spread;

      const glm::vec3 sd = glm::normalize(

          aim + glm::vec3((frand() - 0.5f) * 2.f * sp, (frand() - 0.5f) * 2.f * sp,

                          (frand() - 0.5f) * 2.f * sp));

      const auto eh = shoot_enemies(gun, sd, gun_range);

      if (eh.idx >= 0) {

        damage_enemy(eh.idx, eh.zone, gun_slot.damage);

        state_.tracers.push_back({gun, eh.point, 0.05f});

        state_.impacts.push_back({eh.point, 0.35f});

      } else {

        state_.tracers.push_back({gun, gun + sd * gun_range, 0.05f});

      }

    }

    if (input.fire_secondary && state_.heli_grocket_cd <= 0.f && gunner_rockets.valid) {

      state_.heli_grocket_cd = std::max(0.8f, gunner_rockets.fire_rate * 4.f);

      fire_heli_rocket(gun + nose * 1.0f, input.look_forward, &gunner_rockets);

    }

  } else if (ai_ready) {

    const glm::vec3 ep(state_.enemies[tgt].pos.x, state_.enemies[tgt].pos.y + 1.0f,

                       state_.enemies[tgt].pos.z);

    const glm::vec3 dir = glm::normalize(ep - gun);

    if (state_.heli_gun_cd <= 0.f && gun_slot.valid) {

      state_.heli_gun_cd = gun_cd * 1.2f;

      state_.gunner_engaging = true;

      const glm::vec3 sd = glm::normalize(

          dir + glm::vec3((frand() - 0.5f) * 2.f * gun_spread, (frand() - 0.5f) * 2.f * gun_spread,

                          (frand() - 0.5f) * 2.f * gun_spread));

      const auto eh = shoot_enemies(gun, sd, range + 2.f);

      if (eh.idx >= 0) {

        damage_enemy(eh.idx, eh.zone, gun_slot.damage);

        state_.tracers.push_back({gun, eh.point, 0.05f});

        state_.impacts.push_back({eh.point, 0.35f});

      } else {

        state_.tracers.push_back({gun, gun + sd * range, 0.05f});

      }

    }

    if (state_.heli_grocket_cd <= 0.f && range < 160.f && gunner_rockets.valid) {

      state_.heli_grocket_cd = std::max(2.f, gunner_rockets.fire_rate * 12.f);

      fire_heli_rocket(gun + nose * 1.0f, dir, &gunner_rockets);

    }

  }

} else {

  state_.gunner_target = -1;

  state_.gunner_acquire = 0.f;

}

