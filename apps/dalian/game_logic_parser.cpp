#include "game_logic_parser.hpp"

#include <sstream>

namespace dalian {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

GameLogicDefaults parse_game_logic_init(const std::string& script) {
  GameLogicDefaults out;
  std::istringstream in(script);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string c = lower(cmd);
    if (c == "gamelogic.setdefaultnumberoftickets") {
      int team = 0, tickets = 0;
      if (ls >> team >> tickets) {
        if (team == 1) out.tickets_team1 = tickets;
        if (team == 2) out.tickets_team2 = tickets;
        out.valid = true;
      }
    }
  }
  return out;
}

GameLogicDefaults parse_server_settings(const std::string& script) {
  GameLogicDefaults out;
  std::istringstream in(script);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    if (lower(cmd) == "sv.ticketratio") {
      int ratio = 100;
      if (ls >> ratio) {
        out.ticket_ratio_percent = std::max(1, ratio);
        out.valid = true;
      }
    }
  }
  return out;
}

bool game_logic_parser_self_test() {
  const char* sample = R"(
gameLogic.setDefaultNumberOfTickets 1 250
gameLogic.setDefaultNumberOfTickets 2 200
)";
  const GameLogicDefaults d = parse_game_logic_init(sample);
  return d.valid && d.tickets_team1 == 250 && d.tickets_team2 == 200;
}

}  // namespace dalian
