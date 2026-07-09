if (state_.in_vehicle < 0) return;

Vehicle& v = state_.vehicles[state_.in_vehicle];
const float step = dt > 0.f ? dt : 1.f / 60.f;
const bool boost = input.boost;
const bool driving = state_.player_seat == 0;  // only the pilot/driver flies it

if (v.is_air && v.is_heli) {
  // ---- BF2 rotary-wing flight (thrust-vector + virtual cyclic) ----
  //   W / S       collective: increase (W) / decrease (S) motor — up/down
  //   A / D       yaw (tail rotor) — turn left / right
  //   Mouse X/Y   cyclic attitude targets (holds where you leave it)
  //   Shift       extra rotor power
  // Thrust is applied along the rotor mast; cyclic tilt bleeds altitude via
  // cos(pitch)*cos(roll). Horizon damping recentres when cyclic is neutral.
  const float kHorizonDampAngle = v.horizon_damp_angle;
  const float kHorizontalDampAngleFactor = v.horizontal_damp_factor;
  const float kMaxCollectiveThrust = v.max_collective_thrust;

  float coll_in = 0.f, yaw_in = 0.f;
  float pitch_cmd = 0.f, roll_cmd = 0.f;
  const float max_pitch = v.heli_max_pitch, max_roll = v.heli_max_roll;
  if (driving) {
    if (input.throttle_up) coll_in += 1.f;
    if (input.throttle_down) coll_in -= 1.f;
    if (input.yaw_left) yaw_in += 1.f;
    if (input.yaw_right) yaw_in -= 1.f;
    pitch_cmd = state_.air_pitch_stick * max_pitch;
    roll_cmd = -state_.air_roll_stick * max_roll;
  }
  const float authority = v.rotor_rpm;
  const float thrust_scale = std::max(0.65f, v.max_thrust / 120.f);
  auto approach = [&](float cur, float cmd, float rate) {
    return cur + (cmd - cur) * std::min(1.f, rate * step);
  };
  v.pitch = approach(v.pitch, pitch_cmd * authority, 2.6f);
  v.roll = approach(v.roll, roll_cmd * authority, 2.2f);

  // Weak horizon spring when cyclic is centred — prevents infinite tumble.
  const bool cyclic_neutral =
      std::fabs(state_.air_pitch_stick) < 0.06f && std::fabs(state_.air_roll_stick) < 0.06f;
  if (cyclic_neutral && authority > 0.25f && driving) {
    const float damp = kHorizontalDampAngleFactor * authority * step;
    const float pitch_w = glm::clamp(std::fabs(v.pitch) / kHorizonDampAngle, 0.f, 1.f);
    const float roll_w = glm::clamp(std::fabs(v.roll) / kHorizonDampAngle, 0.f, 1.f);
    if (std::fabs(v.pitch) > 0.5f)
      v.pitch = approach(v.pitch, 0.f, damp * (0.35f + 0.65f * pitch_w));
    if (std::fabs(v.roll) > 0.5f)
      v.roll = approach(v.roll, 0.f, damp * (0.35f + 0.65f * roll_w));
  }

  v.heading += yaw_in * v.heli_yaw_rate * step * authority * 0.55f;

  // Thrust along the tilted mast — pitching forward bleeds altitude (BF2 collective rule).
  const glm::quat q_yaw = glm::angleAxis(glm::radians(v.heading), glm::vec3(0, 1, 0));
  const glm::quat q_pitch = glm::angleAxis(glm::radians(-v.pitch), glm::vec3(1, 0, 0));
  const glm::quat q_roll = glm::angleAxis(glm::radians(-v.roll), glm::vec3(0, 0, 1));
  const glm::quat orient = q_yaw * q_pitch * q_roll;
  const glm::vec3 up_vector = orient * glm::vec3(0.f, 1.f, 0.f);

  float thrust_mag;
  if (driving) {
    thrust_mag = (v.gravity + coll_in * v.collective_gain * thrust_scale) * authority;
    if (input.boost) thrust_mag *= 1.15f * std::sqrt(v.sprint_factor);
  } else {
    thrust_mag = (9.81f - v.vel.y * 2.0f) * authority;
    v.pitch = approach(v.pitch, 0.f, 2.0f);
    v.roll = approach(v.roll, 0.f, 2.0f);
  }
  thrust_mag = glm::clamp(thrust_mag, 0.f, kMaxCollectiveThrust * (boost ? 1.25f : 1.f));

  v.vel += up_vector * thrust_mag * step;
  v.vel.y -= v.gravity * step;

  v.vel.x -= v.vel.x * v.heli_drag_horiz * step;
  v.vel.z -= v.vel.z * v.heli_drag_horiz * step;
  v.vel.y -= v.vel.y * v.heli_drag_vert * step;
  v.vel = glm::clamp(v.vel, glm::vec3(-95.f), glm::vec3(95.f));
  // Integrate horizontally (with building collision), then vertically.
  if (move_vehicle_horiz(v, glm::vec3(v.vel.x, 0.f, v.vel.z) * step)) {
    v.vel.x *= 0.1f;
    v.vel.z *= 0.1f;
  }
  v.pos.y += v.vel.y * step;
  // Land on terrain or a deck below, using the true skid height.
  const float floor_y = air_floor_y(v);
  if (v.pos.y < floor_y) {
    v.pos.y = floor_y;
    if (v.vel.y < 0.f) v.vel.y = 0.f;
    // Settle to level once the skids are on the ground.
    v.pitch = approach(v.pitch, 0.f, 4.0f);
    v.roll = approach(v.roll, 0.f, 4.0f);
  }
  // Diagnostic (BF2_HELIDEMO): pin an airborne bank so a headless capture
  // shows the pitch/roll attitude model. No effect in normal play.
  if (std::getenv("BF2_HELIDEMO")) {
    v.rotor_rpm = 1.f;
    v.pitch = -18.f;
    v.roll = 28.f;
    v.pos.y = params_.world->terrain_height(v.pos.x, v.pos.z) + 25.f;
  }
} else if (v.is_air) {
  // ---- BF2 fixed-wing jet (arcade) ---------------------------------------
  // Ground: W spools engines (no roll until ~55% RPM), double-tap W = afterburner toggle.
  // Air: body-wing lift holds altitude at cruise; stick adds flap lift; bank to turn.
  auto approach = [&](float cur, float cmd, float rate) {
    return cur + (cmd - cur) * std::min(1.f, rate * step);
  };

  constexpr float kV1 = 20.f;          // fallback if tweak missing
  constexpr float kLiftoff = 26.f;
  constexpr float kStall = 20.f;
  constexpr float kCruise = 45.f;
  constexpr float kMaxGround = 52.f;
  constexpr float kMaxAir = 92.f;
  constexpr float kMinRollRpm = 0.55f;
  constexpr float kMaxGroundPitch = 9.f;
  constexpr float kAirborneHeight = 0.75f;
  const float jet_v1 = v.jet_v1 > 1.f ? v.jet_v1 : kV1;
  const float jet_liftoff = v.jet_liftoff > jet_v1 ? v.jet_liftoff : kLiftoff;
  const float jet_stall = v.jet_stall > 1.f ? v.jet_stall : kStall;
  const float jet_cruise = v.jet_cruise > 10.f ? v.jet_cruise : kCruise;
  const float jet_max_ground = v.jet_max_ground > 20.f ? v.jet_max_ground : kMaxGround;
  const float jet_max_air = v.jet_max_air > jet_max_ground ? v.jet_max_air : kMaxAir;
  const float jet_min_roll_rpm = v.jet_min_roll_rpm;
  const float kBodyWingLift = v.wing_body_lift > 0.f ? v.wing_body_lift : 0.62f;
  const float kFlapLift = v.wing_flap_lift > 0.f ? v.wing_flap_lift : 11.f;
  const float kSprintFactor = std::max(1.f, v.sprint_factor);
  const float kThrustScale = std::max(0.7f, v.max_thrust / 120.f);
  const float kSprintDissipation = v.sprint_dissipation > 0.f ? v.sprint_dissipation : 10.f;
  const float kSprintRecover = v.sprint_recover > 0.f ? v.sprint_recover : 30.f;
  const float kGravity = v.gravity > 1.f ? v.gravity : 9.81f;

  float thr_in = 0.f, yaw_in = 0.f;
  if (driving) {
    if (input.throttle_up) thr_in += 1.f;
    if (input.throttle_down) thr_in -= 1.f;
    if (input.yaw_left) yaw_in -= 1.f;
    if (input.yaw_right) yaw_in += 1.f;
  }

  // Landing gear: G toggles; auto-deploy when low/slow; animate tuck 0..1.
  if (input.gear_toggle && driving) {
    if (v.jet_gear_down) {
      if (v.jet_airborne) v.jet_gear_down = false;
    } else {
      v.jet_gear_down = true;
    }
  }
  const float floor_y_gear = air_floor_y(v);
  if (v.jet_airborne && v.pos.y < floor_y_gear + v.gear_up_height) v.jet_gear_down = true;
  if (!v.jet_airborne) v.jet_gear_down = true;
  const float gear_tgt = v.jet_gear_down ? 0.f : 1.f;
  v.jet_gear_anim = approach(v.jet_gear_anim, gear_tgt, gear_tgt > v.jet_gear_anim ? v.gear_anim_up_rate : v.gear_anim_down_rate);

  // Afterburner sprint tank (BF2 sprintLimit/dissipation/recover).
  const bool ab_active = boost && v.jet_sprint > 0.01f;
  if (ab_active) {
    v.jet_sprint = std::max(0.f, v.jet_sprint - step / kSprintDissipation);
  } else {
    v.jet_sprint = std::min(v.sprint_limit, v.jet_sprint + step / kSprintRecover);
  }

  // Commanded throttle ramps slowly; jet_rpm lags behind (engine spool).
  if (thr_in > 0.f)
    v.throttle = glm::clamp(v.throttle + step * 0.32f, 0.f, 1.f);
  else if (thr_in < 0.f)
    v.throttle = glm::clamp(v.throttle - step * 0.45f, 0.f, 1.f);
  else
    v.throttle = glm::max(0.f, v.throttle - step * 0.02f);

  const float spool_up = ab_active ? 0.95f : 0.55f;
  const float spool_down = 0.35f;
  v.jet_rpm = approach(v.jet_rpm, v.throttle, thr_in >= 0.f ? spool_up : spool_down);

  const float floor_y = air_floor_y(v);
  const float hd = glm::radians(v.heading);
  glm::vec3 flat_fwd(std::sin(hd), 0.f, std::cos(hd));
  float horiz_spd = glm::length(glm::vec2(v.vel.x, v.vel.z));

  if (!v.jet_airborne) {
    v.pos.y = floor_y;
    v.vel.y = 0.f;
    v.roll = approach(v.roll, 0.f, 14.f);
    v.jet_airborne = false;
    v.wheels_on_ground = true;
    state_.air_roll_stick = approach(state_.air_roll_stick, 0.f, 16.f);

    // Brakes / idle bleed until engines are spooled enough to roll.
    if (v.jet_rpm < jet_min_roll_rpm) {
      horiz_spd = std::max(0.f, horiz_spd - 18.f * step);
    } else {
      const float roll_auth =
          glm::clamp((v.jet_rpm - jet_min_roll_rpm) / (1.f - jet_min_roll_rpm), 0.f, 1.f);
      const float thrust = (ab_active ? 36.f * kSprintFactor : 22.f) * kThrustScale * roll_auth * v.jet_rpm;
      if (thr_in >= 0.f)
        horiz_spd += thrust * step;
      else
        horiz_spd -= 28.f * step * glm::max(0.25f, horiz_spd / 20.f);
    }
    horiz_spd = glm::clamp(horiz_spd, 0.f, ab_active ? jet_max_ground * 1.12f : jet_max_ground);

    const float rudder = 34.f * glm::clamp(horiz_spd / 14.f, 0.f, 1.f);
    v.heading += yaw_in * rudder * step;
    flat_fwd = glm::vec3(std::sin(glm::radians(v.heading)), 0.f, std::cos(glm::radians(v.heading)));

    // Rotation only after V1; gentle pitch cap prevents tail strikes on the runway.
    float pitch_target = 0.f;
    if (horiz_spd >= jet_v1 && state_.air_pitch_stick > 0.04f) {
      const float rot_t = glm::clamp((horiz_spd - jet_v1) / (jet_liftoff - jet_v1), 0.f, 1.f);
      pitch_target =
          state_.air_pitch_stick * kMaxGroundPitch * rot_t * glm::clamp(v.jet_rpm, 0.6f, 1.f);
    }
    v.pitch = approach(v.pitch, pitch_target, horiz_spd >= jet_v1 ? 2.2f : 8.f);
    v.pitch = glm::clamp(v.pitch, -2.f, kMaxGroundPitch);

    v.vel = flat_fwd * horiz_spd;
    if (move_vehicle_horiz(v, flat_fwd * horiz_spd * step)) horiz_spd *= 0.25f;
    v.vel = flat_fwd * horiz_spd;
    v.pos.y = floor_y;

    // Liftoff: need speed, nose-up, and enough vertical lift — single transition, no flicker.
    const float pitch_rad = glm::radians(v.pitch);
    const float lift_rate =
        std::max(0.f, horiz_spd - jet_v1 * 0.9f) * std::sin(pitch_rad) * 0.55f;
    if (horiz_spd >= jet_liftoff * 0.92f && v.pitch > 3.f && lift_rate > 0.45f) {
      v.jet_airborne = true;
      v.wheels_on_ground = false;
      v.vel.y = lift_rate + std::sin(pitch_rad) * horiz_spd * 0.12f;
      v.pos.y = floor_y + kAirborneHeight * 0.5f;
      v.roll = 0.f;
      state_.air_pitch_stick *= 0.35f;
      state_.air_roll_stick = 0.f;
    }
  } else {
    v.wheels_on_ground = false;

    const bool stick_neutral =
        std::fabs(state_.air_pitch_stick) < 0.06f && std::fabs(state_.air_roll_stick) < 0.06f;
    const float spd_norm = glm::clamp(horiz_spd / jet_cruise, 0.35f, 1.35f);
    // BF2: high speed = wide turn radius; slow down (S) to tighten dogfight turns.
    const float turn_speed_factor = 1.f / (1.f + horiz_spd / std::max(18.f, jet_cruise * 0.55f));
    float ctrl = glm::clamp(horiz_spd / 32.f, 0.45f, 1.f) * turn_speed_factor;

    const float pitch_target = state_.air_pitch_stick * 48.f;
    const float roll_target = -state_.air_roll_stick * 58.f;
    v.pitch = approach(v.pitch, pitch_target, 4.2f * ctrl);
    v.roll = approach(v.roll, roll_target, 3.6f * ctrl);

    if (stick_neutral && driving) {
      const float damp = 3.2f * ctrl * step;
      v.pitch = approach(v.pitch, 0.f, damp * 8.f);
      v.roll = approach(v.roll, 0.f, damp * 10.f);
    }

    const bool stalled = horiz_spd < jet_stall && v.pitch > 8.f;
    if (stalled) {
      ctrl *= 0.3f;
      v.pitch = approach(v.pitch, -8.f, 6.f);
    }

    // Auto-trim: full throttle climbs slightly without stick (BF2 body-wing behaviour).
    if (stick_neutral && driving && !stalled && v.throttle > 0.55f) {
      const float trim = glm::clamp((jet_cruise - horiz_spd) * 0.12f + (v.throttle - 0.6f) * 4.f, -3.f, 5.f);
      v.pitch = approach(v.pitch, trim, 1.8f * step);
    }

    v.pitch = glm::clamp(v.pitch, -55.f, 55.f);
    v.roll = glm::clamp(v.roll, -70.f, 70.f);

    // BF2 turn: bank with mouse, pull back to carve — not A/D wide turns.
    const float bank_turn =
        std::sin(glm::radians(v.roll)) * 52.f * glm::clamp(horiz_spd / 28.f, 0.25f, 1.f) *
        turn_speed_factor;
    const float rudder_air = 8.f * ctrl * glm::clamp(1.f - horiz_spd / jet_cruise, 0.15f, 1.f);
    v.heading += (bank_turn + yaw_in * rudder_air) * step;
    flat_fwd = glm::vec3(std::sin(glm::radians(v.heading)), 0.f, std::cos(glm::radians(v.heading)));

    const float thrust_acc = v.jet_rpm * (ab_active ? 30.f * kSprintFactor : 26.f) * kThrustScale;
    const float drag_base = v.physics_drag > 0.f ? v.physics_drag * 0.0008f : 0.04f;
    const float drag = drag_base + 0.0012f * horiz_spd +
                       (v.jet_gear_anim > 0.05f ? 0.018f * v.jet_gear_anim : 0.f);
    if (input.throttle_down && driving) horiz_spd -= 22.f * step * glm::clamp(horiz_spd / 25.f, 0.3f, 1.f);
    horiz_spd += (thrust_acc - drag * horiz_spd) * step;
    horiz_spd = glm::clamp(horiz_spd, stalled ? 10.f : 16.f,
                           ab_active ? jet_max_air * 1.08f : jet_max_air);

    const float pitch_rad = glm::radians(v.pitch);
  // Body-wing lift rises with speed²; flap lift from stick; gravity always on.
    const float wing_lift =
        kBodyWingLift * spd_norm * spd_norm * kGravity * glm::max(0.35f, std::cos(pitch_rad * 0.55f));
    const float flap_climb = state_.air_pitch_stick * kFlapLift * spd_norm;
    const float pitch_climb = std::sin(pitch_rad) * horiz_spd * 0.32f;
    float vy_acc = wing_lift + flap_climb + pitch_climb - kGravity;
    if (ab_active) vy_acc += 2.5f;
    v.vel.y += vy_acc * step;
    v.vel.y = glm::clamp(v.vel.y, -50.f, 42.f);

    v.vel.x = flat_fwd.x * horiz_spd;
    v.vel.z = flat_fwd.z * horiz_spd;

    if (move_vehicle_horiz(v, glm::vec3(v.vel.x, 0.f, v.vel.z) * step)) {
      horiz_spd *= 0.25f;
      v.vel.x = flat_fwd.x * horiz_spd;
      v.vel.z = flat_fwd.z * horiz_spd;
    }
    v.pos.y += v.vel.y * step;

    if (v.pos.y <= floor_y + 0.35f && v.vel.y <= 0.3f) {
      v.pos.y = floor_y;
      v.vel.y = 0.f;
      v.jet_airborne = false;
      v.wheels_on_ground = true;
      v.roll = approach(v.roll, 0.f, 10.f);
      if (horiz_spd < jet_v1) v.pitch = approach(v.pitch, 0.f, 8.f);
    }
  }
  if (std::getenv("BF2_JETDEMO")) {
    v.throttle = 1.f;
    v.pitch = 12.f;
    v.roll = 18.f;
    v.pos.y = params_.world->terrain_height(v.pos.x, v.pos.z) + 40.f;
  }
} else if (v.is_boat) {
  // Boat / RHIB: same throttle model as ground vehicles but floats on the water
  // surface instead of driving on the seabed heightmap.
  const float water = params_.water_y;
  const float accel = 14.f, max_spd = boost ? 24.f : 16.f, brake = 28.f;
  bool throttling = false;
  if (driving) {
    if (input.throttle_up) {
      v.speed += accel * step;
      throttling = true;
    }
    if (input.throttle_down) {
      v.speed -= accel * step;
      throttling = true;
    }
    const float turn = 55.f * step * std::clamp(std::fabs(v.speed) / 6.f, 0.2f, 1.f);
    const float dir = v.speed >= 0.f ? 1.f : -1.f;
    if (input.yaw_left) v.heading += turn * dir;
    if (input.yaw_right) v.heading -= turn * dir;
  }
  if (!throttling) {
    const float d = brake * 0.55f * step;
    v.speed = std::fabs(v.speed) <= d ? 0.f : v.speed - d * (v.speed > 0 ? 1.f : -1.f);
  }
  v.speed = std::clamp(v.speed, -max_spd * 0.35f, max_spd);
  const float hd = glm::radians(v.heading);
  const glm::vec3 fwd(std::sin(hd), 0.f, std::cos(hd));
  if (move_vehicle_horiz(v, fwd * v.speed * step)) {
    const float impact = std::fabs(v.speed);
    v.speed = 0.f;
    if (impact > 10.f) state_.player_health -= (impact - 10.f) * 2.f;
  }

  auto surface_at = [&](float x, float z) -> float {
    const float terr = params_.world->terrain_height(x, z);
    float s = terr;
    const float probe_top = std::max(v.pos.y, std::max(terr, water)) + 3.f;
    const auto dn = params_.world->raycast({x, probe_top, z}, {0.f, -1.f, 0.f}, 18.f);
    if (dn.hit && dn.normal.y > 0.45f) s = std::max(s, probe_top - dn.distance);
    // Open water: ride the surface, not the seabed.
    if (s < water - 0.05f && terr < water + 0.4f) s = water;
    return s;
  };

  const float t = state_.round_time;
  const float wave =
      0.12f * std::sin(v.pos.x * 0.09f + t * 1.1f) + 0.08f * std::sin(v.pos.z * 0.11f - t * 0.9f);
  const bool on_water = surface_at(v.pos.x, v.pos.z) <= water + 0.15f;
  v.pos.y = surface_at(v.pos.x, v.pos.z) + v.clearance + (on_water ? wave : 0.f);

  if (on_water) {
    v.ground_normal = glm::mix(v.ground_normal, glm::vec3(0.f, 1.f, 0.f),
                               1.f - std::exp(-6.f * step));
    v.pitch = glm::mix(v.pitch, wave * 22.f, 1.f - std::exp(-4.f * step));
    v.roll = glm::mix(v.roll, wave * 14.f, 1.f - std::exp(-4.f * step));
    v.speed *= 1.f - 0.35f * step;
  } else {
    v.ground_normal = glm::vec3(0.f, 1.f, 0.f);
    v.pitch = glm::mix(v.pitch, 0.f, 1.f - std::exp(-6.f * step));
    v.roll = glm::mix(v.roll, 0.f, 1.f - std::exp(-6.f * step));
  }
} else {
  // Ground vehicle: throttle + steer, glued to the terrain.
  const float accel = 18.f, max_spd = boost ? 30.f : 20.f, brake = 34.f;
  bool throttling = false;
  if (driving) {
    if (input.throttle_up) {
      v.speed += accel * step;
      throttling = true;
    }
    if (input.throttle_down) {
      v.speed -= accel * step;
      throttling = true;
    }
    // Steering scales with speed and reverses when backing up.
    const float turn = 75.f * step * std::clamp(std::fabs(v.speed) / 8.f, 0.15f, 1.f);
    const float dir = v.speed >= 0.f ? 1.f : -1.f;
    if (input.yaw_left) v.heading += turn * dir;
    if (input.yaw_right) v.heading -= turn * dir;
    float steer_target = 0.f;
    if (input.yaw_left) steer_target = 0.42f;
    if (input.yaw_right) steer_target = -0.42f;
    v.visual_steer = glm::mix(v.visual_steer, steer_target, 1.f - std::exp(-10.f * step));
  }
  if (!throttling) {  // engine braking / rolling resistance
    const float d = brake * 0.4f * step;
    v.speed = std::fabs(v.speed) <= d ? 0.f : v.speed - d * (v.speed > 0 ? 1.f : -1.f);
  }
  v.speed = std::clamp(v.speed, -max_spd * 0.5f, max_spd);
  const float hd = glm::radians(v.heading);
  const glm::vec3 fwd(std::sin(hd), 0.f, std::cos(hd));
  // Crash into buildings instead of clipping through them.
  if (move_vehicle_horiz(v, fwd * v.speed * step)) {
    const float impact = std::fabs(v.speed);
    v.speed = 0.f;
    if (impact > 12.f) state_.player_health -= (impact - 12.f) * 3.f;  // hard crash hurts
  }
  // Rest on whatever solid surface is directly beneath the chassis, not
  // just the heightmap: casting down catches bridge decks, road props and
  // rail beds laid over the terrain, so the vehicle drives across them
  // instead of sinking to the ground below.
  auto ground_at = [&](float x, float z) -> float {
    const float terr = params_.world->terrain_height(x, z);
    float s = terr;
    const float top = std::max(v.pos.y, terr) + 2.0f;
    const auto dn = params_.world->raycast({x, top, z}, {0.f, -1.f, 0.f}, 12.f);
    if (dn.hit && dn.normal.y > 0.5f) s = std::max(s, top - dn.distance);
    return s;
  };
  const glm::vec3 fdir(std::sin(hd), 0.f, std::cos(hd));
  const glm::vec3 sdir(fdir.z, 0.f, -fdir.x);  // right
  const float L = 2.4f, Wd = 1.3f;             // approx half wheelbase / track
  const float hF = ground_at(v.pos.x + fdir.x * L, v.pos.z + fdir.z * L);
  const float hB = ground_at(v.pos.x - fdir.x * L, v.pos.z - fdir.z * L);
  const float hR = ground_at(v.pos.x + sdir.x * Wd, v.pos.z + sdir.z * Wd);
  const float hL = ground_at(v.pos.x - sdir.x * Wd, v.pos.z - sdir.z * Wd);
  v.pos.y = ground_at(v.pos.x, v.pos.z) + v.clearance;
  // Chassis normal from the four wheel-contact heights (spans the wheelbase
  // so it rides smoothly over bumps, like real suspension).
  const float dFwd = (hF - hB) / (2.f * L);
  const float dSide = (hR - hL) / (2.f * Wd);
  const glm::vec3 target_n =
      glm::normalize(glm::vec3(0.f, 1.f, 0.f) - fdir * dFwd - sdir * dSide);
  const float k = 1.f - std::exp(-8.f * step);  // suspension response
  v.ground_normal = glm::normalize(glm::mix(v.ground_normal, target_n, k));
}

rebuild_vehicle_model(v);
// Keep the soldier "with" the vehicle so re-exit / HUD stays sane.
state_.player.position = {v.pos.x, v.pos.y + state_.player.eye_height, v.pos.z};
state_.player.vertical_velocity = 0.f;
