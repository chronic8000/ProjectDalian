#pragma once

// Multiplayer networking for Project Dalian (milestone 1: two+ players walking
// and shooting in sync).
//
// Design (per the chosen architecture):
//   * Transport   : ENet (reliable + unreliable UDP channels).
//   * Server core : `Net::host()` runs an authoritative relay that can be either
//                   embedded in a client (listen server, local player id 0) or
//                   headless (dedicated: host with no local player rendering).
//   * Clients     : `Net::connect()` simulate their own soldier locally
//                   (client-side prediction) and stream state to the server; the
//                   server assigns ids and broadcasts a snapshot of every player
//                   plus fired-shot events, which remotes render interpolated.
//
// This keeps the hot path allocation-free and the wire format tiny so it is easy
// to extend to vehicles/seats (the pilot/gunner plumbing already maps onto a
// per-player "seat" field) and to tighten into full server authority later.

#include <cstdint>
#include <string>
#include <vector>

namespace bf2 {

struct NetLobbyMember {
  std::uint32_t id = 0;
  std::string name;
  std::uint16_t faction_id = 0;
  bool ready = false;
  bool is_host = false;
};

struct NetLobbyState {
  std::string map_name;
  bool allow_late_join = true;
  bool game_started = false;
  std::vector<NetLobbyMember> members;
};

// Replicated per-player state. Kept flat and POD-friendly for cheap (de)serial.
struct NetPlayer {
  std::uint32_t id = 0;
  float x = 0.f, y = 0.f, z = 0.f;  // feet position (world); vehicle center when driving
  float yaw = 0.f, pitch = 0.f;     // look angles (degrees)
  std::uint8_t anim = 0;            // 0 idle, 1 walk, 2 run (legacy coarse bucket)
  std::uint8_t flags = 0;           // bit0: firing this tick
  std::int16_t health = 100;
  std::uint16_t faction_id = 0;
  bool active = false;  // slot currently occupied

  // Extended replication (v2 wire tail — older clients ignore if packet ends early).
  float vx = 0.f, vy = 0.f, vz = 0.f;
  float anim_time = 0.f;
  std::uint8_t pose = 0;  // dalian::SoldierPose as uint8
  std::int16_t vehicle_id = -1;  // -1 on foot
  float veh_heading = 0.f, veh_pitch = 0.f, veh_roll = 0.f;
  float veh_rotor_rpm = 0.f, veh_rotor_spin = 0.f;

  // Client render-smoothing state (not serialized): the position we currently
  // draw the remote at, eased toward the latest received target each frame.
  float rx = 0.f, ry = 0.f, rz = 0.f, ryaw = 0.f, rpitch = 0.f;
  float rveh_heading = 0.f, rveh_pitch = 0.f, rveh_roll = 0.f;
  float rveh_rotor_rpm = 0.f, rveh_rotor_spin = 0.f;
  bool have_render = false;
};

// A shot fired by a player this frame, replicated so remotes can draw the tracer.
struct NetShot {
  std::uint32_t shooter = 0;
  float ox = 0.f, oy = 0.f, oz = 0.f;  // muzzle origin
  float dx = 0.f, dy = 0.f, dz = 0.f;  // aim direction (unit)
};

class Net {
 public:
  ~Net();

  // Start a server on `port`. `dedicated=true` marks a headless host (no local
  // rendered player). Returns false if ENet fails to initialise/bind.
  bool host(std::uint16_t port, bool dedicated, int max_clients = 16);
  // Connect to a server at host:port. Non-blocking; watch `connected()`.
  bool connect(const std::string& address, std::uint16_t port);

  bool active() const { return role_ != Role::None; }
  bool is_server() const { return role_ == Role::Server; }
  bool is_client() const { return role_ == Role::Client; }
  bool dedicated() const { return dedicated_; }
  bool connected() const { return connected_; }
  std::uint32_t local_id() const { return local_id_; }

  // Pump ENet once per frame: handle (dis)connects and ingest remote data.
  // `dt` drives remote render smoothing. Non-blocking.
  void poll(float dt);

  // Publish the local player's state (+ an optional shot fired this frame).
  // On a client this sends to the server; on a listen server it updates local
  // slot 0 and broadcasts the fresh snapshot to all peers.
  void send_local(const NetPlayer& me, const NetShot* shot);

  // Replicated players (includes everyone; skip your own local_id when drawing).
  const std::vector<NetPlayer>& players() const { return players_; }
  // Drain shot events received since the last call (spawn tracers/impacts).
  std::vector<NetShot> take_shots();
  // Count of connected remote peers (for HUD).
  int peer_count() const;

  // ---- Lobby (pre-game / late join) -----------------------------------------
  bool in_lobby() const { return lobby_mode_; }
  void set_lobby_mode(bool on) { lobby_mode_ = on; }
  const NetLobbyState& lobby() const { return lobby_; }
  bool game_started() const { return lobby_.game_started; }
  bool lobby_joined() const { return lobby_joined_; }

  void send_join_info(const std::string& name, std::uint16_t faction_id);
  void set_ready(bool ready);
  void set_faction(std::uint16_t faction_id);
  void set_lobby_config(bool allow_late_join, const std::string& map_name);
  void host_start_game();

  // Look up lobby metadata (names persist after the match starts).
  std::string lobby_player_name(std::uint32_t id) const;
  std::uint16_t lobby_player_faction(std::uint32_t id) const;

  void shutdown();

 private:
  enum class Role { None, Server, Client };

  NetPlayer* slot(std::uint32_t id, bool create);
  void broadcast_snapshot();
  void handle_packet(const std::uint8_t* data, std::size_t len, void* from_peer);
  void broadcast_lobby();
  NetLobbyMember* lobby_member(std::uint32_t id, bool create);
  void send_lobby_to(void* peer);

  Role role_ = Role::None;
  bool dedicated_ = false;
  bool connected_ = false;
  void* host_ = nullptr;  // ENetHost*
  void* peer_ = nullptr;  // ENetPeer* (client's link to the server)
  std::uint32_t local_id_ = 0;
  std::uint32_t next_id_ = 1;  // server id allocator (0 reserved for the host)
  std::vector<NetPlayer> players_;
  std::vector<NetShot> shots_;      // received shots awaiting drain
  std::vector<NetShot> out_shots_;  // shots to include in the next broadcast
  bool lobby_mode_ = false;
  bool lobby_joined_ = false;
  NetLobbyState lobby_;
  std::string local_name_;
  std::uint16_t local_faction_ = 0;
  bool local_ready_ = false;
};

}  // namespace bf2
