#include "conquest_sim.hpp"

#include <algorithm>
#include <cmath>

namespace dalian {

TeamId dominant_team(const ConquestPresence& p) {
  if (p.team1 && !p.team2) return TeamId::Team1;
  if (p.team2 && !p.team1) return TeamId::Team2;
  return TeamId::Neutral;
}

void step_control_point_capture(ControlPoint& cp, const ConquestPresence& presence, float dt,
                                const ConquestConfig& cfg, bool& flipped) {
  flipped = false;
  if (!cp.capturable) return;

  const TeamId dom = dominant_team(presence);
  if (dom == TeamId::Neutral) {
    if (cp.capturer != TeamId::Neutral && cp.capture_progress > 0.f) {
      cp.capture_progress =
          std::max(0.f, cp.capture_progress - cfg.uncontested_decay_rate * dt);
      if (cp.capture_progress <= 0.f) cp.capturer = TeamId::Neutral;
    }
    return;
  }

  if (dom == cp.owner) {
    cp.capturer = cp.owner;
    cp.capture_progress = 1.f;
    return;
  }

  if (cp.capturer != dom) {
    cp.capturer = dom;
    cp.capture_progress = 0.f;
  }

  const float rate =
      cp.owner == TeamId::Neutral ? cfg.neutral_capture_rate : cfg.enemy_capture_rate;
  cp.capture_progress = std::min(1.f, cp.capture_progress + rate * dt);
  if (cp.capture_progress >= 1.f) {
    cp.owner = dom;
    cp.capturer = dom;
    cp.capture_progress = 1.f;
    flipped = true;
  }
}

void step_ticket_bleed(TicketState& tickets, const ControlPoint* points, std::size_t count,
                       float dt, const ConquestConfig& cfg) {
  int t1_owned = 0;
  int t2_owned = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!points[i].capturable) continue;
    if (points[i].owner == TeamId::Team1) ++t1_owned;
    else if (points[i].owner == TeamId::Team2) ++t2_owned;
  }

  const float bleed = cfg.bleed_per_flag * dt;
  if (t1_owned > t2_owned) {
    tickets.team2_bleed_accum += (t1_owned - t2_owned) * bleed;
    const int whole = static_cast<int>(tickets.team2_bleed_accum);
    if (whole > 0) {
      tickets.team2_tickets -= whole;
      tickets.team2_bleed_accum -= static_cast<float>(whole);
    }
  } else if (t2_owned > t1_owned) {
    tickets.team1_bleed_accum += (t2_owned - t1_owned) * bleed;
    const int whole = static_cast<int>(tickets.team1_bleed_accum);
    if (whole > 0) {
      tickets.team1_tickets -= whole;
      tickets.team1_bleed_accum -= static_cast<float>(whole);
    }
  }
  tickets.team1_tickets = std::max(0, tickets.team1_tickets);
  tickets.team2_tickets = std::max(0, tickets.team2_tickets);
}

bool apply_death_ticket(TicketState& tickets, TeamId team, const ConquestConfig& cfg) {
  if (cfg.death_ticket_cost <= 0) return false;
  if (team == TeamId::Team1) {
    tickets.team1_tickets = std::max(0, tickets.team1_tickets - cfg.death_ticket_cost);
    return true;
  }
  if (team == TeamId::Team2) {
    tickets.team2_tickets = std::max(0, tickets.team2_tickets - cfg.death_ticket_cost);
    return true;
  }
  return false;
}

TeamId check_ticket_victory(const TicketState& tickets) {
  if (tickets.team1_tickets <= 0 && tickets.team2_tickets > 0) return TeamId::Team2;
  if (tickets.team2_tickets <= 0 && tickets.team1_tickets > 0) return TeamId::Team1;
  if (tickets.team1_tickets <= 0 && tickets.team2_tickets <= 0) return TeamId::Neutral;
  return TeamId::Neutral;
}

bool conquest_sim_self_test() {
  ConquestConfig cfg{};
  ControlPoint cp;
  cp.owner = TeamId::Neutral;
  cp.radius = 15.f;

  ConquestPresence p;
  p.team1 = true;
  bool flipped = false;
  for (int i = 0; i < 10; ++i) {
    step_control_point_capture(cp, p, 1.f, cfg, flipped);
  }
  if (cp.owner != TeamId::Team1) return false;

  TicketState t{100, 100};
  ControlPoint owned[2];
  owned[0].capturable = true;
  owned[0].owner = TeamId::Team1;
  owned[1].capturable = true;
  owned[1].owner = TeamId::Team1;
  for (int i = 0; i < 50; ++i) step_ticket_bleed(t, owned, 2, 1.f, cfg);
  if (t.team2_tickets >= 100) return false;
  if (check_ticket_victory(t) != TeamId::Neutral) return false;

  // High-frequency bleed must not drain a ticket every frame (old ceil bug).
  TicketState fast{100, 100};
  ControlPoint adv[3];
  adv[0].capturable = true;
  adv[0].owner = TeamId::Team2;
  adv[1].capturable = true;
  adv[1].owner = TeamId::Team2;
  adv[2].capturable = true;
  adv[2].owner = TeamId::Team1;
  for (int i = 0; i < 120; ++i) step_ticket_bleed(fast, adv, 3, 1.f / 60.f, cfg);
  if (fast.team1_tickets <= 50) return false;

  t.team1_tickets = 10;
  t.team2_tickets = 0;
  return check_ticket_victory(t) == TeamId::Team1;
}

}  // namespace dalian
