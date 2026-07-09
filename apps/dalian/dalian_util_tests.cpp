#include "conquest_sim.hpp"
#include "game_snapshot.hpp"
#include "hitbox_zones.hpp"
#include "map_conquest_parser.hpp"
#include "minimap_projector.hpp"
#include "radial_menu.hpp"
#include "tweak_parser.hpp"

#include <cstdio>

int main() {
  int failed = 0;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++failed;
  };

  check("tweak_parser", dalian::tweak_parser_self_test());
  check("minimap_projector", dalian::minimap_projector_self_test());
  check("radial_menu", dalian::radial_menu_self_test());
  check("hitbox_zones", dalian::hitbox_zones_self_test());
  check("conquest_sim", dalian::conquest_sim_self_test());
  check("game_snapshot", dalian::snapshot::game_snapshot_self_test());
  check("map_conquest_parser", dalian::map_conquest_parser_self_test());

  return failed == 0 ? 0 : 1;
}
