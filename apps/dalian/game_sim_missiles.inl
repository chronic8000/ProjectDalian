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
      glm::vec3 dir0;
      if (horiz > 1.f) {
        // Artillery loft: climb hard so the round is visible before the dive.
        const float loft = std::clamp(horiz * 0.5f, 110.f, 320.f);
        const float up = loft / std::max(horiz, 1.f);
        // Keep a strong upward bias even on short shots (was nearly horizontal).
        const float fwd = horiz < 80.f ? 0.28f : 0.85f;
        dir0 = glm::normalize(glm::normalize(flat) * fwd + glm::vec3(0.f, std::max(up, 1.15f), 0.f));
      } else {
        dir0 = glm::vec3(0.f, 1.f, 0.f);
      }
      spawn_missile_from_profile(launch, dir0, prof, true);
      if (!state_.missiles.empty()) {
        ActiveMissile& am = state_.missiles.back();
        am.m.target = aim_point;
        am.m.has_target = true;
        // Ballistic climb first — guidance arms later so you see the loft + drop.
        am.m.guided = false;
        am.homing_enemy = -1;  // stay on the map mark — don't steal lock to distant AI
        am.detonation_fx = 1;
        // Visible artillery arc (was too fast / fused mid-air before the dive).
        am.m.velocity = dir0 * 22.f;
        am.m.max_speed = 58.f;
        am.m.boost_accel = 32.f;
        am.m.boost_time = 3.6f;
        am.m.turn_rate = 0.85f;
        am.m.life = 35.f;
        am.m.gravity = 9.81f * 0.9f;
        am.m.drag = 0.00045f;
        // Artillery / Hawk SAM: big but not map-clearing. (Was 38m/320 — nuke.)
        am.explosion_radius = 12.f;
        am.explosion_damage = 145.f;
        am.smoke_timer = 0.04f;
        am.guide_arm_age = 2.4f;  // start steering after the climb
        am.fuse_arm_age = 1.6f;   // no proximity boom during boost
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
  // Hawk / Car-SAM: ballistic loft, then arm guidance for the dive.
  if (am.detonation_fx == 1 && am.m.has_target && am.homing_enemy < 0) {
    if (am.m.age >= am.guide_arm_age) {
      am.m.guided = true;
      am.m.has_target = true;
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
  // Map-guided SAMs: wait for fuse arm, then only boom near the ground mark
  // (old fuse fired at +35m AGL while still diving — looked like no drop).
  if (!detonate && am.m.has_target && am.homing_enemy < 0 && am.detonation_fx == 1) {
    if (am.m.age >= am.fuse_arm_age) {
      const float dx = am.m.target.x - am.m.position.x;
      const float dz = am.m.target.z - am.m.position.z;
      const float horiz2 = dx * dx + dz * dz;
      const bool near_ground = am.m.position.y < am.m.target.y + 8.f;
      const bool diving = am.m.velocity.y < 8.f;
      if (horiz2 < 9.f * 9.f && near_ground && diving) {
        detonate = true;
        boom = am.m.target;
      } else if (glm::length(am.m.target - am.m.position) < 5.f) {
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
