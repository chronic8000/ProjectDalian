if (state_.in_vehicle < 0) return;

Vehicle& v = state_.vehicles[state_.in_vehicle];
if (v.destroyed) {
  state_.in_vehicle = -1;
  state_.player_seat = 0;
  return;
}
const float step = dt > 0.f ? dt : 1.f / 60.f;
const bool boost = input.boost;
const bool driving = state_.player_seat == 0;  // only the pilot/driver flies it

// BF2 masses are ~4k–12k kg. Scale arcade accelerations so heavier airframes lag.
const float mass_scale = [&]() {
  const float m = v.physics_mass > 100.f ? v.physics_mass : 6500.f;
  constexpr float kRefMass = 6500.f;
  return glm::clamp(kRefMass / m, 0.55f, 1.4f);
}();

// Jet dogfight authority: peak near cruise (BF2 ~300–315 HUD), weak when stalled
// or afterburning. Replaces the old monotonic "slower = better" factor.
auto jet_turn_authority = [](float speed, float cruise, float max_air) {
  const float sweet = std::max(12.f, cruise);
  const float ratio = speed / sweet;
  if (ratio < 0.5f) {
    const float t = glm::clamp(ratio / 0.5f, 0.f, 1.f);
    return glm::mix(0.12f, 0.42f, t);
  }
  if (ratio <= 1.f) {
    const float t = (ratio - 0.5f) / 0.5f;
    return glm::mix(0.42f, 1.f, t);
  }
  const float hi = std::max(sweet + 1.f, max_air);
  const float t = glm::clamp((speed - sweet) / (hi - sweet), 0.f, 1.f);
  return glm::mix(1.f, 0.22f, t);
};

if (v.is_air && v.is_heli) {
  // ---- BF2 rotary-wing: cyclic → rotor AoA → tilted thrust → body follows ----
  // Refractor model (research §5): mouse does NOT pitch the chassis directly.
  // Stick tilts the rotor disc; thrust along that disc swings the heavy body.
  const float g = v.gravity * v.gravity_modifier;
  const float inertia = std::max(0.45f, v.inertia_modifier);
  float coll_in = 0.f;
  float yaw_in = state_.air_yaw_stick;
  if (driving) {
    if (input.throttle_up) coll_in += 1.f;
    if (input.throttle_down) coll_in -= 1.f;
    // Mouse wheel → collective (c_PIThrottle analog).
    if (std::fabs(input.air_throttle_delta) > 1e-4f) coll_in += input.air_throttle_delta * 0.55f;
    coll_in = glm::clamp(coll_in, -1.f, 1.f);
  }
  const float authority = v.rotor_rpm;
  const float thrust_scale = std::max(0.65f, v.max_thrust / 120.f);
  auto approach = [&](float cur, float cmd, float rate) {
    return cur + (cmd - cur) * std::min(1.f, rate * step);
  };

  // --- Rotor Angle of Attack (virtual cyclic) ---
  const float max_aoa = v.max_aoa;
  float aoa_pitch_tgt = v.default_aoa;
  float aoa_roll_tgt = 0.f;
  if (driving) {
    aoa_pitch_tgt = v.default_aoa + state_.air_pitch_stick * max_aoa;
    aoa_roll_tgt = -state_.air_roll_stick * max_aoa;
  }
  const bool cyclic_neutral =
      std::fabs(state_.air_pitch_stick) < 0.06f && std::fabs(state_.air_roll_stick) < 0.06f;
  if (cyclic_neutral && driving) {
    // DecreaseAngleToZero — rotor disc returns to hover trim when stick released.
    aoa_pitch_tgt = v.default_aoa;
    aoa_roll_tgt = 0.f;
    const float zrate = v.decrease_aoa_to_zero / inertia;
    v.rotor_aoa_pitch = approach(v.rotor_aoa_pitch, aoa_pitch_tgt, zrate);
    v.rotor_aoa_roll = approach(v.rotor_aoa_roll, aoa_roll_tgt, zrate);
  } else {
    const float aspeed = v.attack_speed / inertia;
    v.rotor_aoa_pitch = approach(v.rotor_aoa_pitch, aoa_pitch_tgt, aspeed);
    v.rotor_aoa_roll = approach(v.rotor_aoa_roll, aoa_roll_tgt, aspeed);
  }
  v.rotor_aoa_pitch = glm::clamp(v.rotor_aoa_pitch, -max_aoa - 2.f, max_aoa + v.default_aoa + 2.f);
  v.rotor_aoa_roll = glm::clamp(v.rotor_aoa_roll, -max_aoa - 2.f, max_aoa + 2.f);

  // Body attitude lags the rotor disc (pendulum / heavy swing).
  const float body_follow = (2.1f / inertia) * authority;
  const float pitch_cmd = glm::clamp(v.rotor_aoa_pitch * (v.heli_max_pitch / std::max(8.f, max_aoa)),
                                     -v.heli_max_pitch, v.heli_max_pitch);
  const float roll_cmd = glm::clamp(v.rotor_aoa_roll * (v.heli_max_roll / std::max(8.f, max_aoa)),
                                    -v.heli_max_roll, v.heli_max_roll);
  v.pitch = approach(v.pitch, pitch_cmd * authority, body_follow);
  v.roll = approach(v.roll, roll_cmd * authority, body_follow * 0.95f);

  // HorizonDampAngle / HorizontalDampAngleFactor — auto-level when cyclic neutral.
  if (cyclic_neutral && authority > 0.25f && driving) {
    const float damp = v.horizontal_damp_factor * authority * step / inertia;
    const float pitch_w = glm::clamp(std::fabs(v.pitch) / v.horizon_damp_angle, 0.f, 1.f);
    const float roll_w = glm::clamp(std::fabs(v.roll) / v.horizon_damp_angle, 0.f, 1.f);
    if (std::fabs(v.pitch) > 0.5f)
      v.pitch = approach(v.pitch, 0.f, damp * (0.4f + 0.6f * pitch_w) * 8.f);
    if (std::fabs(v.roll) > 0.5f)
      v.roll = approach(v.roll, 0.f, damp * (0.4f + 0.6f * roll_w) * 8.f);
  }

  // Tail rotor: AirFlowEffect — yaw authority drops with forward speed.
  const float horiz_spd = glm::length(glm::vec2(v.vel.x, v.vel.z));
  const float yaw_air =
      glm::mix(1.f, v.airflow_yaw_factor, glm::clamp(horiz_spd / 42.f, 0.f, 1.f));
  v.heading += yaw_in * v.heli_yaw_rate * step * authority * 0.55f * yaw_air;

  // Thrust along rotor mast (tilted by AoA in body space, then world).
  const glm::quat q_yaw = glm::angleAxis(glm::radians(v.heading), glm::vec3(0, 1, 0));
  const glm::quat q_pitch = glm::angleAxis(glm::radians(-v.pitch), glm::vec3(1, 0, 0));
  const glm::quat q_roll = glm::angleAxis(glm::radians(-v.roll), glm::vec3(0, 0, 1));
  const glm::quat orient = q_yaw * q_pitch * q_roll;
  // Disc tilt relative to body: small additional offset from AoA vs body attitude.
  const float disc_p = glm::radians(v.rotor_aoa_pitch - v.pitch * 0.35f);
  const float disc_r = glm::radians(v.rotor_aoa_roll - v.roll * 0.35f);
  glm::vec3 mast_local = glm::normalize(glm::vec3(std::sin(disc_r), 1.f, -std::sin(disc_p)));
  const glm::vec3 thrust_dir = glm::normalize(orient * mast_local);

  float thrust_mag;
  if (driving) {
    thrust_mag = (g + coll_in * v.collective_gain * thrust_scale) * authority;
    if (input.boost) thrust_mag *= 1.15f * std::sqrt(v.sprint_factor);
  } else {
    thrust_mag = (g - v.vel.y * 2.0f) * authority;
    v.pitch = approach(v.pitch, 0.f, 2.0f);
    v.roll = approach(v.roll, 0.f, 2.0f);
    v.rotor_aoa_pitch = approach(v.rotor_aoa_pitch, v.default_aoa, 2.0f);
    v.rotor_aoa_roll = approach(v.rotor_aoa_roll, 0.f, 2.0f);
  }
  thrust_mag = glm::clamp(thrust_mag, 0.f, v.max_collective_thrust * (boost ? 1.25f : 1.f));
  thrust_mag *= mass_scale;

  v.vel += thrust_dir * thrust_mag * step;
  v.vel.y -= g * step;

  // Anisotropic drag (DragModifier) + slide when tilted.
  const float tilt = 1.f - std::cos(glm::radians(v.pitch)) * std::cos(glm::radians(v.roll));
  const float slide = glm::clamp(tilt * 1.8f, 0.f, 1.f);
  float drag_h = v.heli_drag_horiz * glm::mix(1.f, 0.28f, slide);
  float drag_v = v.heli_drag_vert;
  drag_h *= 0.5f * (v.drag_modifier.x + v.drag_modifier.z);
  drag_v *= v.drag_modifier.y;

  // DampHorizontalVelFactor — bleed forward momentum when stick is neutral (hover gun).
  if (cyclic_neutral && driving && authority > 0.4f) {
    drag_h += v.damp_horizontal_vel * 0.55f;
  }

  v.vel.x -= v.vel.x * drag_h * step;
  v.vel.z -= v.vel.z * drag_h * step;
  v.vel.y -= v.vel.y * drag_v * step;
  v.vel = glm::clamp(v.vel, glm::vec3(-95.f), glm::vec3(95.f));

  if (move_vehicle_horiz(v, glm::vec3(v.vel.x, 0.f, v.vel.z) * step)) {
    v.vel.x *= 0.1f;
    v.vel.z *= 0.1f;
  }
  v.pos.y += v.vel.y * step;
  const float floor_y = air_floor_y(v);
  if (v.pos.y < floor_y) {
    v.pos.y = floor_y;
    if (v.vel.y < 0.f) v.vel.y = 0.f;
    v.pitch = approach(v.pitch, 0.f, 4.0f);
    v.roll = approach(v.roll, 0.f, 4.0f);
    v.rotor_aoa_pitch = approach(v.rotor_aoa_pitch, v.default_aoa, 4.0f);
    v.rotor_aoa_roll = approach(v.rotor_aoa_roll, 0.f, 4.0f);
  }
  if (std::getenv("BF2_HELIDEMO")) {
    v.rotor_rpm = 1.f;
    v.pitch = -18.f;
    v.roll = 28.f;
    v.pos.y = params_.world->terrain_height(v.pos.x, v.pos.z) + 25.f;
  }
} else if (v.is_air) {
  // ---- BF2 fixed-wing: v² wing/flap lift, speed-scaled controls, energy model ----
  auto approach = [&](float cur, float cmd, float rate) {
    return cur + (cmd - cur) * std::min(1.f, rate * step);
  };

  constexpr float kV1 = 20.f;
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
  const float kGravity = (v.gravity > 1.f ? v.gravity : 9.81f) * v.gravity_modifier;
  const float inertia = std::max(0.45f, v.inertia_modifier);
  const float yaw_in = state_.air_yaw_stick;

  float thr_in = 0.f;
  if (driving) {
    if (input.throttle_up) thr_in += 1.f;
    if (input.throttle_down) thr_in -= 1.f;
  }

  // c_PIThrottle analog (mouse wheel notches buffered between fixed ticks).
  if (driving && std::fabs(input.air_throttle_delta) > 1e-4f) {
    v.throttle = glm::clamp(v.throttle + input.air_throttle_delta * 0.14f, 0.f, 1.f);
  }

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
  v.jet_gear_anim =
      approach(v.jet_gear_anim, gear_tgt,
               gear_tgt > v.jet_gear_anim ? v.gear_anim_up_rate : v.gear_anim_down_rate);

  const bool ab_active = boost && v.jet_sprint > 0.01f;
  if (ab_active) {
    v.jet_sprint = std::max(0.f, v.jet_sprint - step / kSprintDissipation);
  } else {
    v.jet_sprint = std::min(v.sprint_limit, v.jet_sprint + step / kSprintRecover);
  }

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

    if (v.jet_rpm < jet_min_roll_rpm) {
      horiz_spd = std::max(0.f, horiz_spd - 18.f * step);
    } else {
      const float roll_auth =
          glm::clamp((v.jet_rpm - jet_min_roll_rpm) / (1.f - jet_min_roll_rpm), 0.f, 1.f);
      const float thrust =
          (ab_active ? 36.f * kSprintFactor : 22.f) * kThrustScale * roll_auth * v.jet_rpm *
          mass_scale;
      if (thr_in >= 0.f)
        horiz_spd += thrust * step;
      else
        horiz_spd -= 28.f * step * glm::max(0.25f, horiz_spd / 20.f);
    }
    horiz_spd = glm::clamp(horiz_spd, 0.f, ab_active ? jet_max_ground * 1.12f : jet_max_ground);

    const float rudder = 34.f * glm::clamp(horiz_spd / 14.f, 0.f, 1.f);
    v.heading += yaw_in * rudder * step;
    flat_fwd = glm::vec3(std::sin(glm::radians(v.heading)), 0.f, std::cos(glm::radians(v.heading)));

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
    const float airspeed = glm::length(v.vel);
    // Dynamic pressure proxy (v/cruise)² — controls AND lift scale with this.
    const float q = glm::clamp((airspeed / jet_cruise) * (airspeed / jet_cruise), 0.08f, 2.2f);
    const float turn_speed_factor = jet_turn_authority(airspeed, jet_cruise, jet_max_air);
    float ctrl = turn_speed_factor * glm::clamp(q, 0.2f, 1.35f) / inertia;

    // Flap deflection → attitude. Torque arms from PositionOffset (§7.1):
    // elevators aft (Z) pitch the nose; ailerons outboard (X) roll the jet.
    const float elev_arm = glm::clamp(-v.elevator_z_offset / 5.5f, 0.55f, 1.6f);
    const float ail_arm = glm::clamp(v.aileron_x_offset / 3.2f, 0.55f, 1.6f);
    const float pitch_target = state_.air_pitch_stick * 48.f;
    const float roll_target = -state_.air_roll_stick * 58.f;
    v.pitch = approach(v.pitch, pitch_target, v.jet_pitch_rate * ctrl * elev_arm);
    v.roll = approach(v.roll, roll_target, v.jet_roll_rate * ctrl * ail_arm);

    if (stick_neutral && driving && v.automatic_reset > 0.2f) {
      const float damp = 3.2f * ctrl * v.automatic_reset * step;
      v.pitch = approach(v.pitch, 0.f, damp * 8.f);
      v.roll = approach(v.roll, 0.f, damp * 10.f);
    }

    const bool stalled = horiz_spd < jet_stall && (v.pitch > 8.f || airspeed < jet_stall * 0.85f);
    if (stalled) {
      ctrl *= 0.28f;
      v.pitch = approach(v.pitch, -12.f, 7.f);
    }

    if (stick_neutral && driving && !stalled && v.throttle > 0.55f) {
      const float trim =
          glm::clamp((jet_cruise - horiz_spd) * 0.12f + (v.throttle - 0.6f) * 4.f, -3.f, 5.f);
      v.pitch = approach(v.pitch, trim, 1.8f * step);
    }

    v.pitch = glm::clamp(v.pitch, -55.f, 55.f);
    v.roll = glm::clamp(v.roll, -70.f, 70.f);

    const float bank_turn = std::sin(glm::radians(v.roll)) * v.jet_bank_turn_gain *
                            glm::clamp(horiz_spd / 28.f, 0.25f, 1.f) * turn_speed_factor;
    const float rudder_air =
        10.f * ctrl * glm::clamp(1.f - horiz_spd / (jet_cruise * 1.2f), 0.12f, 1.f) *
        glm::mix(1.f, 0.45f, glm::clamp(horiz_spd / jet_cruise, 0.f, 1.f));
    v.heading += (bank_turn + yaw_in * rudder_air) * step;
    flat_fwd = glm::vec3(std::sin(glm::radians(v.heading)), 0.f, std::cos(glm::radians(v.heading)));

    float thrust_acc =
        v.jet_rpm * (ab_active ? 30.f * kSprintFactor : 26.f) * kThrustScale * mass_scale;
    // Drag + induced drag from high flap / bank (energy bleed in turns).
    const float drag_base = v.physics_drag > 0.f ? v.physics_drag * 0.0008f : 0.04f;
    const float load =
        1.f + 0.55f * std::fabs(state_.air_pitch_stick) + 0.35f * std::fabs(std::sin(glm::radians(v.roll)));
    float drag = drag_base * (0.65f * v.drag_modifier.z + 0.35f) + 0.0012f * horiz_spd * load;
    drag += (v.jet_gear_anim > 0.05f ? 0.018f * v.jet_gear_anim : 0.f);
    if (input.throttle_down && driving)
      horiz_spd -= 22.f * step * glm::clamp(horiz_spd / 25.f, 0.3f, 1.f);
    horiz_spd += (thrust_acc - drag * horiz_spd) * step;
    horiz_spd = glm::clamp(horiz_spd, stalled ? 8.f : 14.f,
                           ab_active ? jet_max_air * 1.08f : jet_max_air);

    const float pitch_rad = glm::radians(v.pitch);
    // WingLift + FlapLift×stick, scaled by dynamic pressure, clamped by RegulateToLift.
    float lift_raw = (kBodyWingLift + state_.air_pitch_stick * (kFlapLift * 0.045f)) * q;
    const float lift_cap = v.regulate_to_lift * v.wing_to_regulator;
    lift_raw = std::min(lift_raw, lift_cap);
    const float wing_lift = lift_raw * kGravity * glm::max(0.28f, std::cos(pitch_rad * 0.55f));
    const float flap_climb = state_.air_pitch_stick * kFlapLift * glm::clamp(q, 0.25f, 1.4f) * 0.55f;
    const float pitch_climb = std::sin(pitch_rad) * horiz_spd * 0.32f;
    float vy_acc = wing_lift + flap_climb + pitch_climb - kGravity;
    if (ab_active) vy_acc += 2.5f;
    if (stalled) vy_acc -= 6.f;
    v.vel.y += vy_acc * step;
    v.vel.y = glm::clamp(v.vel.y, -55.f, 42.f);

    // Vertical drag modifier.
    v.vel.y -= v.vel.y * (0.04f * v.drag_modifier.y) * step;

    v.vel.x = flat_fwd.x * horiz_spd;
    v.vel.z = flat_fwd.z * horiz_spd;

    if (move_vehicle_horiz(v, glm::vec3(v.vel.x, 0.f, v.vel.z) * step)) {
      horiz_spd *= 0.25f;
      v.vel.x = flat_fwd.x * horiz_spd;
      v.vel.z = flat_fwd.z * horiz_spd;
    }
    v.pos.y += v.vel.y * step;

    if (v.pos.y <= floor_y + 0.35f && v.vel.y <= 0.8f) {
      // Landing gear spring/damper (§7.2) — absorb sink rate instead of a hard stop.
      const float penet = std::max(0.f, floor_y - v.pos.y);
      v.vel.y += (penet * v.gear_spring - v.vel.y * v.gear_damping) * step;
      if (v.pos.y < floor_y) v.pos.y = floor_y;
      if (v.vel.y > 0.f) v.vel.y *= 0.35f;
      if (v.vel.y <= 0.35f && penet < 0.08f) {
        v.vel.y = 0.f;
        v.jet_airborne = false;
        v.wheels_on_ground = true;
        v.roll = approach(v.roll, 0.f, 10.f);
        if (horiz_spd < jet_v1) v.pitch = approach(v.pitch, 0.f, 8.f);
      }
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
    // +heading = clockwise (right turn when going forward).
    if (input.yaw_left) v.heading -= turn * dir;
    if (input.yaw_right) v.heading += turn * dir;
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
    // +heading = clockwise (right). Matches BF2 after LH view correction.
    if (input.yaw_left) v.heading -= turn * dir;
    if (input.yaw_right) v.heading += turn * dir;
    float steer_target = 0.f;
    if (input.yaw_left) steer_target = -0.42f;
    if (input.yaw_right) steer_target = 0.42f;
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
  //
  // Ignore hangar/canopy roofs: a hit far above the expected wheel contact
  // (one side roof, one side ground) was tipping tanks ~45° on enter.
  const float expect_surf = v.pos.y - v.clearance;
  auto ground_at = [&](float x, float z) -> float {
    const float terr = params_.world->terrain_height(x, z);
    float s = terr;
    const float band_lo = std::min(terr, expect_surf) - 1.5f;
    const float band_hi = expect_surf + 1.25f;
    const float feet = expect_surf + 1.5f;
    const float support = params_.world->support_height(x, z, feet, 4.0f);
    if (support > terr + 0.2f && support >= band_lo && support <= band_hi)
      s = std::max(s, support);
    const float top = expect_surf + 1.75f;
    const auto dn = params_.world->raycast({x, top, z}, {0.f, -1.f, 0.f}, 10.f);
    if (dn.hit) {
      float ny = dn.normal.y;
      if (ny < 0.f) ny = -ny;
      const float hit_y = top - dn.distance;
      if (ny > 0.5f && hit_y >= band_lo && hit_y <= band_hi) s = std::max(s, hit_y);
    }
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
  float dFwd = (hF - hB) / (2.f * L);
  float dSide = (hR - hL) / (2.f * Wd);
  // Cap slope so a bad sample can't stand the vehicle on its side.
  dFwd = std::clamp(dFwd, -0.55f, 0.55f);
  dSide = std::clamp(dSide, -0.55f, 0.55f);
  const glm::vec3 target_n =
      glm::normalize(glm::vec3(0.f, 1.f, 0.f) - fdir * dFwd - sdir * dSide);
  const float k = 1.f - std::exp(-8.f * step);  // suspension response
  v.ground_normal = glm::normalize(glm::mix(v.ground_normal, target_n, k));
}

rebuild_vehicle_model(v);
// Keep the soldier "with" the vehicle so re-exit / HUD stays sane.
state_.player.position = {v.pos.x, v.pos.y + state_.player.eye_height, v.pos.z};
state_.player.vertical_velocity = 0.f;
