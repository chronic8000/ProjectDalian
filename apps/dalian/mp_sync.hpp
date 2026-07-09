#pragma once

#include "engine/net/net.hpp"
#include "game_sim_types.hpp"
#include "soldier_anim.hpp"

namespace bf2 {
struct CharacterController;
}

namespace dalian {

class GameSim;

// Publish local soldier + vehicle state for replication.
void fill_net_player(bf2::NetPlayer& me, const bf2::CharacterController& player, float yaw, float pitch,
                     float anim_time, SoldierPose pose, int in_vehicle,
                     const std::vector<Vehicle>& vehicles, float player_health,
                     std::uint16_t faction_id, bool net_fired);

// Lock unoccupied vehicles to spawn; apply remote drivers' vehicle transforms.
void apply_mp_vehicle_sync(std::vector<Vehicle>& vehicles, const bf2::Net& net, int local_in_vehicle,
                           GameSim& sim);

}  // namespace dalian
