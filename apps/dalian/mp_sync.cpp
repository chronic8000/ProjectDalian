#include "mp_sync.hpp"

#include "engine/physics/physics_world.hpp"
#include "game_sim.hpp"

#include <algorithm>
#include <cmath>

namespace dalian {

void fill_net_player(bf2::NetPlayer& me, const bf2::CharacterController& player, float yaw, float pitch,
                     float anim_time, SoldierPose pose, int in_vehicle,
                     const std::vector<Vehicle>& vehicles, float player_health,
                     std::uint16_t faction_id, bool net_fired) {
  me.yaw = yaw;
  me.pitch = pitch;
  me.anim_time = anim_time;
  me.pose = static_cast<std::uint8_t>(pose);
  me.faction_id = faction_id;
  me.flags = net_fired ? 1 : 0;
  me.health = static_cast<std::int16_t>(std::clamp(player_health, -32000.f, 32000.f));
  me.vehicle_id = static_cast<std::int16_t>(in_vehicle);

  if (in_vehicle >= 0 && in_vehicle < static_cast<int>(vehicles.size())) {
    const Vehicle& v = vehicles[static_cast<std::size_t>(in_vehicle)];
    me.x = v.pos.x;
    me.y = v.pos.y;
    me.z = v.pos.z;
    me.vx = v.vel.x;
    me.vy = v.vel.y;
    me.vz = v.vel.z;
    me.veh_heading = v.heading;
    me.veh_pitch = v.pitch;
    me.veh_roll = v.roll;
    me.veh_rotor_rpm = v.rotor_rpm;
    me.veh_rotor_spin = v.rotor_spin;
    me.anim = 0;
  } else {
    me.x = player.position.x;
    me.y = player.position.y - player.eye_height;
    me.z = player.position.z;
    me.vx = player.desired_velocity.x;
    me.vy = player.desired_velocity.y;
    me.vz = player.desired_velocity.z;
    me.veh_heading = 0.f;
    me.veh_pitch = 0.f;
    me.veh_roll = 0.f;
    me.veh_rotor_rpm = 0.f;
    me.veh_rotor_spin = 0.f;
    const float spd =
        std::sqrt(me.vx * me.vx + me.vz * me.vz);
    me.anim = spd > 8.f ? 2 : (spd > 0.3f ? 1 : 0);
  }
}

static void reset_vehicle_to_spawn(Vehicle& v, GameSim& sim) {
  v.pos = v.spawn_pos;
  v.heading = v.spawn_heading;
  v.pitch = v.spawn_pitch;
  v.roll = v.spawn_roll;
  v.vel = glm::vec3(0.f);
  v.speed = 0.f;
  v.throttle = 0.f;
  v.rotor_rpm = 0.f;
  v.rotor_spin = 0.f;
  v.jet_rpm = 0.f;
  v.jet_sprint = 1.f;
  v.jet_airborne = false;
  v.wheels_on_ground = true;
  v.destroyed = false;
  v.health = 1000.f;
  sim.sync_vehicle_transform(v);
}

void apply_mp_vehicle_sync(std::vector<Vehicle>& vehicles, const bf2::Net& net, int local_in_vehicle,
                           GameSim& sim) {
  if (!net.active() || vehicles.empty()) return;

  std::vector<bool> driven(vehicles.size(), false);
  if (local_in_vehicle >= 0 && local_in_vehicle < static_cast<int>(vehicles.size()))
    driven[static_cast<std::size_t>(local_in_vehicle)] = true;

  for (const auto& rp : net.players()) {
    if (!rp.active || rp.id == net.local_id()) continue;
    if (rp.vehicle_id < 0 || rp.vehicle_id >= static_cast<int>(vehicles.size())) continue;
    driven[static_cast<std::size_t>(rp.vehicle_id)] = true;
    Vehicle& v = vehicles[static_cast<std::size_t>(rp.vehicle_id)];
    v.pos = glm::vec3(rp.rx, rp.ry, rp.rz);
    v.heading = rp.rveh_heading;
    v.pitch = rp.rveh_pitch;
    v.roll = rp.rveh_roll;
    v.rotor_rpm = rp.rveh_rotor_rpm;
    v.rotor_spin = rp.rveh_rotor_spin;
    v.vel = glm::vec3(rp.vx, rp.vy, rp.vz);
    sim.sync_vehicle_transform(v);
  }

  for (std::size_t i = 0; i < vehicles.size(); ++i) {
    if (driven[i]) continue;
    if (vehicles[i].destroyed) {
      reset_vehicle_to_spawn(vehicles[i], sim);
      continue;
    }
    Vehicle& v = vehicles[i];
    const glm::vec3 d = v.pos - v.spawn_pos;
    if (glm::dot(d, d) > 0.04f || std::fabs(v.heading - v.spawn_heading) > 0.5f ||
        std::fabs(v.speed) > 0.05f || v.rotor_rpm > 0.02f) {
      reset_vehicle_to_spawn(v, sim);
    }
  }
}

}  // namespace dalian
