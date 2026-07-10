if (input.drone_mode || input.deploy_open) return;

if (input.seat_switch >= 0 && state_.in_vehicle >= 0 &&
    state_.in_vehicle < static_cast<int>(state_.vehicles.size())) {
  Vehicle& v = state_.vehicles[state_.in_vehicle];
  const int want = input.seat_switch;
  if (want < static_cast<int>(v.seats.size()) && want != state_.player_seat) {
    const int prev = v.seats[want].occupant;
    v.seats[state_.player_seat].occupant = prev == 1 ? 1 : -1;
    v.seats[want].occupant = 0;
    state_.player_seat = want;
  }
}

if (!input.enter_exit) return;

if (state_.in_vehicle >= 0 && state_.in_vehicle < static_cast<int>(state_.vehicles.size())) {
  Vehicle& v = state_.vehicles[state_.in_vehicle];
  const float hd = glm::radians(v.heading);
  const glm::vec3 side(std::cos(hd), 0.f, -std::sin(hd));
  glm::vec3 want = v.pos + side * 4.0f;
  want.y = v.pos.y;
  const glm::vec3 safe = find_safe_spawn(want);
  state_.player.position = {safe.x, safe.y + state_.player.eye_height + 0.3f, safe.z};
  state_.player.vertical_velocity = 0.f;
  if (v.is_air) {
    v.pitch = 0.f;
    v.roll = 0.f;
    v.vel = glm::vec3(0.f);
    rebuild_vehicle_model(v);
  }
  for (auto& s : v.seats) s.occupant = -1;
  events_.vehicle_exited = true;
  events_.exit_yaw_deg = 90.f - v.heading;
  state_.air_pitch_stick = state_.air_roll_stick = 0.f;
  state_.in_vehicle = -1;
  state_.player_seat = 0;
  return;
}

int best = -1;
float best_d2 = 6.0f * 6.0f;
for (std::size_t i = 0; i < state_.vehicles.size(); ++i) {
  if (state_.vehicles[i].destroyed) continue;
  if (state_.vehicles[i].mesh_key.find("vehicles/") == std::string::npos) continue;
  const glm::vec3 d(state_.vehicles[i].pos.x - state_.player.position.x, 0.f,
                    state_.vehicles[i].pos.z - state_.player.position.z);
  const float d2 = d.x * d.x + d.z * d.z;
  if (d2 < best_d2) {
    best_d2 = d2;
    best = static_cast<int>(i);
  }
}
if (best < 0) return;

state_.in_vehicle = best;
state_.player_seat = 0;
state_.air_pitch_stick = state_.air_roll_stick = 0.f;
Vehicle& nv = state_.vehicles[best];
for (auto& s : nv.seats) s.occupant = -1;
if (!nv.seats.empty()) nv.seats[0].occupant = 0;
if (nv.seats.size() > 1) nv.seats[1].occupant = 1;
// Start level so the first ground sample can't inherit a tipped normal.
if (!nv.is_air && !nv.is_boat) {
  nv.ground_normal = glm::vec3(0.f, 1.f, 0.f);
  rebuild_vehicle_model(nv);
}
if (nv.is_air) {
  events_.capture_mouse = true;
  events_.discard_mouse_delta = true;
  nv.pitch = 0.f;
  nv.roll = 0.f;
  nv.vel = glm::vec3(0.f);
  nv.throttle = 0.f;
  nv.wheels_on_ground = true;
  state_.air_input_grace = 0.35f;
}
