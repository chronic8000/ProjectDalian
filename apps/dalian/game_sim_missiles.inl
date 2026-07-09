state_.missile_reload = std::max(0.f, state_.missile_reload - dt);

const bool want_at = input.launch_at && params_.at_missile.valid;
const bool want_sam = input.launch_missile && !want_at && params_.sam_missile.valid;
if ((want_at || want_sam) && state_.missile_reload <= 0.f) {
  const ProjectileProfile& prof = want_at ? params_.at_missile : params_.sam_missile;
  state_.missile_reload = want_at ? 3.0f : 1.4f;
  const glm::vec3& eye = input.eye;
  const glm::vec3& front = input.look_forward;
  glm::vec3 launch = eye + front * 0.8f - glm::vec3(0.f, 0.2f, 0.f);
  float best_d2 = 70.f * 70.f;
  for (const auto& v : state_.vehicles) {
    const glm::vec3 d = v.origin - eye;
    const float d2 = glm::dot(d, d);
    if (d2 < best_d2) {
      best_d2 = d2;
      launch = v.origin + glm::vec3(0.f, 2.6f, 0.f);
    }
  }
  if (params_.world) {
    const auto th = params_.world->raycast({eye.x, eye.y, eye.z}, {front.x, front.y, front.z}, 2000.f);
    const float terr = th.hit ? th.distance : 2000.f;
    const auto eh = shoot_enemies(eye, front, terr);
    glm::vec3 aim_point;
  bool homing = prof.guided && !want_at;
    if (eh.idx >= 0) {
      aim_point = glm::vec3(state_.enemies[eh.idx].pos.x, state_.enemies[eh.idx].pos.y + 1.0f,
                            state_.enemies[eh.idx].pos.z);
    } else if (th.hit) {
      aim_point = glm::vec3(th.point.x, th.point.y, th.point.z);
    } else {
      aim_point = eye + front * 800.f;
      homing = false;
    }
    glm::vec3 dir0 = aim_point - launch;
    if (glm::length(dir0) < 1e-3f) dir0 = front;
    dir0 = glm::normalize(dir0);
    spawn_missile_from_profile(launch, dir0, prof, homing);
    if (eh.idx >= 0 && homing && !state_.missiles.empty()) {
      state_.missiles.back().homing_enemy = eh.idx;
      state_.missiles.back().m.target = aim_point;
      state_.missiles.back().m.has_target = true;
    } else if (th.hit && homing && !state_.missiles.empty()) {
      state_.missiles.back().m.target = aim_point;
      state_.missiles.back().m.has_target = true;
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
      std::fprintf(stderr, "[missile] LAUNCH kind=%s pos=%.1f,%.1f,%.1f\n",
                   want_at ? "AT" : "SAM", am.m.position.x, am.m.position.y, am.m.position.z);
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
  am.m.update(dt);

  const glm::vec3 seg = am.m.position - am.prev_pos;
  const float seg_len = glm::length(seg);
  bool detonate = false;
  glm::vec3 boom = am.m.position;
  if (seg_len > 1e-4f) {
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
  if (!detonate) {
    const float gh = params_.world->terrain_height(am.m.position.x, am.m.position.z);
    if (am.m.position.y <= gh + 0.2f) {
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
    am.smoke_timer = 0.012f;
    if (state_.smoke.size() < 8000) {
      Smoke trail;
      trail.p = am.m.position - am.m.forward() * 0.6f;
      trail.vel = -am.m.forward() * 4.f;
      trail.life = 1.2f;
      trail.size = 0.35f;
      trail.kind = 1;
      state_.smoke.push_back(trail);
    }
  }

  if (detonate) {
    am.m.alive = false;
    explode_at(boom, am.explosion_radius, am.explosion_damage);
    if (std::getenv("BF2_MISSILE_DEBUG")) {
      std::fprintf(stderr, "[missile] DETONATE at %.1f,%.1f,%.1f age=%.2f\n", boom.x, boom.y, boom.z,
                   am.m.age);
    }
  }
}
state_.missiles.erase(std::remove_if(state_.missiles.begin(), state_.missiles.end(),
                                     [](const ActiveMissile& a) { return !a.m.alive; }),
                      state_.missiles.end());
