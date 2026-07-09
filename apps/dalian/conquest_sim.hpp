#pragma once

#include "conquest_types.hpp"

namespace dalian {

struct ConquestConfig {
  int starting_tickets = 150;
  float capture_radius = 15.f;
  float neutral_capture_rate = 0.12f;   // progress per second
  float enemy_capture_rate = 0.06f;   // flipping an owned flag
  float uncontested_decay_rate = 0.5f;  // progress lost per second when empty
  float bleed_per_flag = 0.08f;         // tickets/sec per flag advantage
  int death_ticket_cost = 1;
};

// Pure conquest rules — unit-testable without GameSim / PhysicsWorld.
struct ConquestPresence {
  bool team1 = false;
  bool team2 = false;
};

TeamId dominant_team(const ConquestPresence& p);
void step_control_point_capture(ControlPoint& cp, const ConquestPresence& presence, float dt,
                                const ConquestConfig& cfg, bool& flipped);
void step_ticket_bleed(TicketState& tickets, const ControlPoint* points, std::size_t count,
                       float dt, const ConquestConfig& cfg);
bool apply_death_ticket(TicketState& tickets, TeamId team, const ConquestConfig& cfg);
TeamId check_ticket_victory(const TicketState& tickets);

bool conquest_sim_self_test();

}  // namespace dalian
