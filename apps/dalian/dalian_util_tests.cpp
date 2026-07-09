#include "conquest_sim.hpp"
#include "game_snapshot.hpp"
#include "hitbox_zones.hpp"
#include "game_logic_parser.hpp"
#include "map_conquest_parser.hpp"
#include "minimap_projector.hpp"
#include "radial_menu.hpp"
#include "tweak_parser.hpp"

#include "engine/core/collision_resolver.hpp"
#include "vehicle_air_profile.hpp"
#include "vehicle_weapon_profile.hpp"
#include "projectile_profile.hpp"
#include "ambient_emitter.hpp"

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
  check("game_logic_parser", dalian::game_logic_parser_self_test());
  check("vehicle_air_profile", dalian::vehicle_air_profile_self_test());
  check("vehicle_weapon_profile", dalian::vehicle_weapon_profile_self_test());
  check("projectile_profile", dalian::projectile_profile_self_test());
  check("ambient_emitter", dalian::ambient_emitter_self_test());
  {
    dalian::AmbientEmitter e;
    e.kind = 2;
    e.pos = {1.f, 2.f, 3.f};
    const auto lights = dalian::collect_scene_lights({e});
    check("scene_lights", lights.size() == 1 && lights[0].pos.y == 2.f);
  check("collision_resolver", bf2::collision_resolver_self_test());
  }

  return failed == 0 ? 0 : 1;
}
