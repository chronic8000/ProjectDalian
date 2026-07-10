state_.missile_reload = std::max(0.f, state_.missile_reload - dt);
const bool want_at = input.launch_at && params_.at_missile.valid;
const bool want_sam = input.launch_missile && !want_at && params_.sam_missile.valid;
if ((want_at || want_sam) && state_.missile_reload <= 0.f) {
  const ProjectileProfile& prof = want_at ? params_.at_missile : params_.sam_missile;
  state_.missile_reload = want_at ? 3.0f : 1.4f;
  const glm::vec3& eye = input.eye;
  const glm::vec3& front = input.look_forward;
  // AT: shoulder-fire from the soldier. SAM: car roof or fixed emplacement origin.
  glm::vec3 launch = eye + front * 0.8f - glm::vec3(0.f, 0.2f, 0.f);
  if (want_sam) {
    float best_d2 = 28.f * 28.f;
    bool found_car = false;
    if (input.has_sam_launch_origin) {
      launch = input.sam_launch_origin;
      found_car = true;
    } else {
      for (const auto& v : state_.vehicles) {
        if (v.is_air) continue;
        if (v.mesh_key.find("vehicles/") == std::string::npos) continue;
        const glm::vec3 d = v.pos - eye;
        const float d2 = glm::dot(d, d);
        if (d2 < best_d2) {
          best_d2 = d2;
          launch = v.pos + glm::vec3(0.f, 2.6f, 0.f);
          found_car = true;
        }
      }
    }
    if (!found_car) {
      state_.missile_reload = 0.15f;
    } else if (params_.world) {
      // Destination comes from the tactical map click when provided; else look-ray fallback.
      glm::vec3 aim_point;
      if (input.has_missile_target) {
        aim_point = input.missile_target;
      } else {
        const auto th =
            params_.world->raycast({eye.x, eye.y, eye.z}, {front.x, front.y, front.z}, 2000.f);
        aim_point = th.hit ? glm::vec3(th.point.x, th.point.y, th.point.z) : eye + front * 800.f;
      }
      // Snap Y to terrain so map clicks land on the ground.
      const float gh = params_.world->terrain_height(aim_point.x, aim_point.z);
      aim_point.y = std::max(aim_point.y, gh + 1.5f);
      glm::vec3 flat = aim_point - launch;
      flat.y = 0.f;
      const float horiz = glm::length(flat);
      // Mortar-style: nearly straight up (tiny lean toward the mark). Old loft was
      // ~horizontal so the round flew into mountains then steered sideways.
      glm::vec3 dir0(0.f, 1.f, 0.f);
      if (horiz > 1.f) {
        dir0 = glm::normalize(glm::normalize(flat) * 0.08f + glm::vec3(0.f, 1.f, 0.f));
      }
      const float loft_h = std::clamp(horiz * 0.4f, 140.f, 300.f);
      spawn_missile_from_profile(launch, dir0, prof, true);
      if (!state_.missiles.empty()) {
        ActiveMissile& am = state_.missiles.back();
        am.impact_pos = aim_point;
        am.has_impact = true;
        am.loft_height = loft_h;
        am.arty_phase = 0;
        am.m.target = aim_point;
        am.m.has_target = true;
        am.m.guided = false;
        am.homing_enemy = -1;
        am.detonation_fx = 1;
        // Vertical boost climb, then high transit, then dive.
        am.m.velocity = dir0 * 30.f;
        am.m.max_speed = 75.f;
        am.m.boost_accel = 48.f;
        am.m.boost_time = 2.6f;
        am.m.turn_rate = 1.15f;
        am.m.life = 40.f;
        am.m.gravity = 9.81f * 1.05f;
        am.m.drag = 0.0004f;
        am.explosion_radius = 12.f;
        am.explosion_damage = 145.f;
        am.smoke_timer = 0.04f;
        am.guide_arm_age = 3.8f;  // force transit if still climbing
        am.fuse_arm_age = 2.0f;
      }
      if (params_.missile_headless_demo && !state_.missiles.empty()) {
        ActiveMissile& am = state_.missiles.back();
        am.m.position = eye;
        am.prev_pos = eye;
        am.m.boost_accel = 130.f;
        am.m.max_speed = 260.f;
      }
      if (std::getenv("BF2_MISSILE_DEBUG") && !state_.missiles.empty()) {
        const auto& am = state_.missiles.back();
        std::fprintf(stderr, "[missile] LAUNCH kind=SAM pos=%.1f,%.1f,%.1f tgt=%.1f,%.1f,%.1f\n",
                     am.m.position.x, am.m.position.y, am.m.position.z, aim_point.x, aim_point.y,
                     aim_point.z);
      }
    }
  } else if (params_.world) {
    const auto th = params_.world->raycast({eye.x, eye.y, eye.z}, {front.x, front.y, front.z}, 2000.f);
    const float terr = th.hit ? th.distance : 2000.f;
    const auto eh = shoot_enemies(eye, front, terr);
    glm::vec3 aim_point;
    if (eh.idx >= 0) {
      aim_point = glm::vec3(state_.enemies[eh.idx].pos.x, state_.enemies[eh.idx].pos.y + 1.0f,
                            state_.enemies[eh.idx].pos.z);
    } else if (th.hit) {
      aim_point = glm::vec3(th.point.x, th.point.y, th.point.z);
    } else {
      aim_point = eye + front * 800.f;
    }
    glm::vec3 dir0 = aim_point - launch;
    if (glm::length(dir0) < 1e-3f) dir0 = front;
    dir0 = glm::normalize(dir0);
    spawn_missile_from_profile(launch, dir0, prof, false);
    if (params_.missile_headless_demo && !state_.missiles.empty()) {
      ActiveMissile& am = state_.missiles.back();
      am.m.position = eye;
      am.prev_pos = eye;
      am.m.boost_accel = 130.f;
      am.m.max_speed = 260.f;
    }
    if (std::getenv("BF2_MISSILE_DEBUG") && !state_.missiles.empty()) {
      const auto& am = state_.missiles.back();
      std::fprintf(stderr, "[missile] LAUNCH kind=AT pos=%.1f,%.1f,%.1f\n", am.m.position.x,
                   am.m.position.y, am.m.position.z);
    }
  }
}
if (!params_.world) return;
for (auto& am : state_.missiles) {
  if (!am.m.alive) continue;
  if (am.homing_enemy >= 0 && am.homing_enemy < static_cast<int>(state_.enemies.size())) {
    const Enemy& te = state_.enemies[am.homing_enemy];
    if (te.alive) {
      am.m.target = glm::vec3(te.pos.x, te.pos.y + 1.0f, te.pos.z);
    } else {
      am.homing_enemy = -1;
    }
  }
  am.prev_pos = am.m.position;
  // Hawk / Car-SAM mortar profile: up → slide over mark → tip over / dive.
  if (am.detonation_fx == 1 && am.has_impact && am.homing_enemy < 0) {
    const float dx = am.impact_pos.x - am.m.position.x;
    const float dz = am.impact_pos.z - am.m.position.z;
    const float horiz2 = dx * dx + dz * dz;
    if (am.arty_phase == 0) {
      am.m.guided = false;
      // Past apex (falling) or climb timeout → transit high over the mark.
      if ((am.m.age > 1.4f && am.m.velocity.y < 4.f) || am.m.age >= am.guide_arm_age) {
        am.arty_phase = 1;
        const float hold_y =
            std::max(am.m.position.y, am.impact_pos.y + am.loft_height * 0.65f);
        am.m.target = glm::vec3(am.impact_pos.x, hold_y, am.impact_pos.z);
        am.m.has_target = true;
        am.m.guided = true;
        am.m.turn_rate = 1.05f;
      }
    } else if (am.arty_phase == 1) {
      am.m.guided = true;
      const float hold_y =
          std::max(am.m.position.y * 0.98f, am.impact_pos.y + am.loft_height * 0.45f);
      am.m.target = glm::vec3(am.impact_pos.x, hold_y, am.impact_pos.z);
      // Over the mark (or close enough) → tip over into a steep dive.
      if (horiz2 < 40.f * 40.f || am.m.age > am.guide_arm_age + 10.f) {
        am.arty_phase = 2;
        am.m.target = am.impact_pos;
        am.m.turn_rate = 1.55f;
      }
    } else {
      am.m.guided = true;
      am.m.target = am.impact_pos;
      am.m.has_target = true;
    }
  } else if (am.detonation_fx == 1 && am.m.has_target && am.homing_enemy < 0) {
    // Fallback if impact_pos wasn't set.
    if (am.m.age >= am.guide_arm_age) {
      am.m.guided = true;
    } else {
      am.m.guided = false;
    }
  }
  am.m.update(dt);
  const glm::vec3 seg = am.m.position - am.prev_pos;
  const float seg_len = glm::length(seg);
  bool detonate = false;
  glm::vec3 boom = am.m.position;
  const bool sam_armed = am.detonation_fx != 1 || am.m.age >= am.fuse_arm_age;
  if (seg_len > 1e-4f && sam_armed) {
    const glm::vec3 sd = seg / seg_len;
    const auto hit = params_.world->raycast({am.prev_pos.x, am.prev_pos.y, am.prev_pos.z},
                                            {seg.x, seg.y, seg.z}, seg_len);
    const float terr_d = hit.hit ? hit.distance : 1e30f;
    const auto eh = shoot_enemies(am.prev_pos, sd, seg_len);
    if (eh.idx >= 0 && eh.dist < terr_d) {
      detonate = true;
      boom = eh.point;
    } else if (hit.hit) {
      detonate = true;
      boom = glm::vec3(hit.point.x, hit.point.y, hit.point.z);
    }
  }
  if (!detonate && am.homing_enemy >= 0 && am.homing_enemy < static_cast<int>(state_.enemies.size())) {
    const Enemy& te = state_.enemies[am.homing_enemy];
    const glm::vec3 chest(te.pos.x, te.pos.y + 1.f, te.pos.z);
    if (te.alive && glm::length(chest - am.m.position) < 2.5f) {
      detonate = true;
      boom = am.m.position;
    }
  }
  // Map-guided SAMs: wait for fuse arm, then boom near the ground mark on dive.
  if (!detonate && am.has_impact && am.homing_enemy < 0 && am.detonation_fx == 1) {
    if (am.m.age >= am.fuse_arm_age && am.arty_phase >= 2) {
      const float dx = am.impact_pos.x - am.m.position.x;
      const float dz = am.impact_pos.z - am.m.position.z;
      const float horiz2 = dx * dx + dz * dz;
      const bool near_ground = am.m.position.y < am.impact_pos.y + 10.f;
      const bool diving = am.m.velocity.y < 5.f;
      if (horiz2 < 10.f * 10.f && near_ground && diving) {
        detonate = true;
        boom = am.impact_pos;
      } else if (glm::length(am.impact_pos - am.m.position) < 5.f) {
        detonate = true;
        boom = am.impact_pos;
      }
    }
  } else if (!detonate && am.m.has_target && am.homing_enemy < 0 && am.detonation_fx == 1) {
    if (am.m.age >= am.fuse_arm_age) {
      const float dx = am.m.target.x - am.m.position.x;
      const float dz = am.m.target.z - am.m.position.z;
      const float horiz2 = dx * dx + dz * dz;
      if (horiz2 < 9.f * 9.f && am.m.position.y < am.m.target.y + 8.f && am.m.velocity.y < 8.f) {
        detonate = true;
        boom = am.m.target;
      }
    }
  } else if (!detonate && am.m.has_target && am.homing_enemy < 0 &&
             glm::length(am.m.target - am.m.position) < 4.5f) {
    detonate = true;
    boom = am.m.target;
  }
  if (!detonate) {
    const float gh = params_.world->terrain_height(am.m.position.x, am.m.position.z);
    // Ignore ground scrapes during the boost climb (launcher / apron).
    const bool armed = am.detonation_fx != 1 || am.m.age >= am.fuse_arm_age;
    if (armed && am.m.position.y <= gh + 0.2f) {
      detonate = true;
      boom = glm::vec3(am.m.position.x, gh, am.m.position.z);
    }
  }
  if (!am.m.alive) {
    detonate = true;
    boom = am.m.position;
  }
  am.smoke_timer -= dt;
  if (am.smoke_timer <= 0.f) {
    // Car-SAM: thinner trail so the rocket body stays visible.
    am.smoke_timer = am.detonation_fx == 1 ? 0.055f : 0.018f;
    if (am.detonation_fx == 1) {
      spawn_missile_trail_fx(state_.smoke, am.m.position - am.m.forward() * 0.8f, am.m.forward(),
                             0.35f);
    } else {
      spawn_missile_trail_fx(state_.smoke, am.m.position - am.m.forward() * 0.55f, am.m.forward());
    }
  }
  if (detonate) {
    am.m.alive = false;
    if (am.detonation_fx == 1) {
      spawn_igla_detonation_fx(state_.smoke, state_.explosions, boom,
                               glm::clamp(am.explosion_radius / 8.f, 1.15f, 2.4f));
      explode_at(boom, am.explosion_radius, am.explosion_damage, false);
      events_.play_artillery_explosion = true;
      events_.artillery_explosion_pos = boom;
    } else {
      explode_at(boom, am.explosion_radius, am.explosion_damage);
    }
    if (std::getenv("BF2_MISSILE_DEBUG")) {
      std::fprintf(stderr, "[missile] DETONATE at %.1f,%.1f,%.1f age=%.2f\n", boom.x, boom.y, boom.z,
                   am.m.age);
    }
  }
}
state_.missiles.erase(std::remove_if(state_.missiles.begin(), state_.missiles.end(),
                                     [](const ActiveMissile& a) { return !a.m.alive; }),
                      state_.missiles.end());
