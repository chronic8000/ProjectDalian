#pragma once

#include "conquest_types.hpp"
#include "game_sim_state.hpp"

namespace dalian {

struct GameState;

namespace snapshot {

snapshot::TeamId to_snapshot_team(dalian::TeamId team);

// Pack the live simulation bag into a net-replication snapshot (no SDL / audio).
GameState build_snapshot(const dalian::GameState& sim, std::uint32_t local_player_id = 1,
                         float player_yaw_deg = 0.f);

bool game_snapshot_self_test();

}  // namespace snapshot
}  // namespace dalian
