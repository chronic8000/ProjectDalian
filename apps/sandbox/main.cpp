#include <iostream>

#include "engine/formats/terrain/terrain_loader.hpp"
#include "engine/net/network_stub.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/script/gameplay_script.hpp"

int main() {
  bf2::GameplayScript script;
  script.initialize();
  script.bind_float("health", 100.f);
  script.bind_float("ammo", 30.f);
  script.call("fire_weapon");

  bf2::PhysicsWorld physics;
  std::vector<std::uint8_t> flat_terrain(512 * 512 * 2, 0);
  physics.set_terrain(bf2::TerrainLoader::load_raw_heightmap(flat_terrain, 512, 512, 0.1f));
  bf2::PhysicsBody player{};
  player.position = {256.f, 5.f, 256.f};
  physics.add_body(player);
  physics.step(1.f / 60.f);

  bf2::NetworkStub net;
  net.start_server(16567);
  bf2::NetEntityState state{};
  state.id = 1;
  state.position[0] = player.position.x;
  state.position[1] = player.position.y;
  state.position[2] = player.position.z;
  net.publish_state(state);

  std::cout << "BF2 Respawn sandbox\n";
  std::cout << "  ammo: " << script.get_float("ammo") << '\n';
  std::cout << "  player_y: " << physics.bodies().front().position.y << '\n';
  std::cout << "  replicated_entities: " << net.poll_states().size() << '\n';
  return 0;
}
