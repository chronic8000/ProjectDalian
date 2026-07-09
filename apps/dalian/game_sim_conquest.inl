if (!state_.match_started || state_.match_over) return;

state_.round_time += dt;

for (auto& cp : state_.control_points) {
  const TeamId prev_owner = cp.owner;
  ConquestPresence presence{};
  const glm::vec3 player_feet(state_.player.position.x,
                              state_.player.position.y - state_.player.eye_height,
                              state_.player.position.z);
  if (xz_distance_sq(player_feet, cp.pos) <= cp.radius * cp.radius) {
    if (state_.player_team == TeamId::Team1) presence.team1 = true;
    else if (state_.player_team == TeamId::Team2) presence.team2 = true;
  }

  for (const auto& en : state_.enemies) {
    if (!en.alive) continue;
    if (xz_distance_sq(en.pos, cp.pos) <= cp.radius * cp.radius) {
      if (en.team == TeamId::Team1) presence.team1 = true;
      else if (en.team == TeamId::Team2) presence.team2 = true;
    }
  }

  bool flipped = false;
  step_control_point_capture(cp, presence, dt, conquest_cfg_, flipped);
  if (flipped) {
    events_.control_point_captured = true;
    events_.captured_cp_id = cp.id;
    events_.captured_cp_owner = cp.owner;
    if (cp.owner == state_.player_team) {
      events_.voice_cue = "flag_captured_friendly";
    } else if (prev_owner == state_.player_team) {
      events_.voice_cue = "flag_lost_friendly";
    }
  }
}

if (!params_.multiplayer || params_.connected_humans >= 2 || state_.round_time >= 90.f) {
  step_ticket_bleed(state_.tickets, state_.control_points.data(), state_.control_points.size(), dt,
                    conquest_cfg_);
}

if (!state_.team1_ticket_warned && state_.player_team == TeamId::Team1 &&
    state_.tickets.team1_tickets > 0 &&
    state_.tickets.team1_tickets <= conquest_cfg_.starting_tickets / 5) {
  state_.team1_ticket_warned = true;
  if (events_.voice_cue.empty()) events_.voice_cue = "tickets_low_friendly";
}
if (!state_.team2_ticket_warned && state_.player_team == TeamId::Team2 &&
    state_.tickets.team2_tickets > 0 &&
    state_.tickets.team2_tickets <= conquest_cfg_.starting_tickets / 5) {
  state_.team2_ticket_warned = true;
  if (events_.voice_cue.empty()) events_.voice_cue = "tickets_low_friendly";
}

const TeamId victor = check_ticket_victory(state_.tickets);
if (victor != TeamId::Neutral) {
  state_.match_over = true;
  state_.winning_team = victor;
  events_.match_over = true;
  events_.winning_team = victor;
  events_.voice_cue = victor == state_.player_team ? "victory_friendly" : "defeat_friendly";
}
