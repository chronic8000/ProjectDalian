#pragma once

#include "engine/net/net.hpp"
#include "game_sim_types.hpp"
#include "soldier_anim.hpp"

namespace bf2 {
struct CharacterController;
}

namespace dalian {

class GameSim;

void fill_net_player(bf2::NetPlayer& me, const bf2::CharacterController& player, float yaw, float pitch,
                     float anim_time, SoldierPose pose, int in_vehicle,
                     const std::vector<Vehicle>& vehicles, float player_health,
                     std::uint16_t faction_id, bool net_fired, float send_dt,
                     std::uint32_t input_seq, int player_seat = 0);

bf2::NetInput fill_net_input(const PlayerInput& inp, float yaw, float pitch, std::uint32_t& seq);

void apply_mp_vehicle_sync(std::vector<Vehicle>& vehicles, const bf2::Net& net, int local_in_vehicle,
                           GameSim& sim);

}  // namespace dalian
