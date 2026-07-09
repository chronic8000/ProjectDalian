#pragma once

#include <string>

namespace dalian {

struct GameLogicDefaults {
  int tickets_team1 = 250;
  int tickets_team2 = 250;
  int ticket_ratio_percent = 100;  // sv.ticketRatio from ServerSettings.con
  float ticket_loss_per_min = 4.f;  // retail default when fully dominant
  bool valid = false;
};

// Parse mod-root GameLogicInit.con (setDefaultNumberOfTickets, etc.).
GameLogicDefaults parse_game_logic_init(const std::string& script);
GameLogicDefaults parse_server_settings(const std::string& script);

bool game_logic_parser_self_test();

}  // namespace dalian
