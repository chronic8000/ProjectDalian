#pragma once



// Multiplayer networking for Project Dalian.

//

// Architecture (listen-server relay + snapshot interpolation):

//   * Transport   : ENet (reliable + unreliable UDP channels).

//   * Server      : authoritative relay; assigns ids, rebroadcasts on every

//                   client state/input update and on a fixed 60 Hz timer.

//   * Clients     : local client-side prediction; stream state + compact input

//                   each frame. Remotes are rendered via a timestamped snapshot

//                   ring buffer interpolated ~100 ms in the past (Gaffer model).



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

  std::string map_server_zip;

  bool allow_late_join = true;

  bool game_started = false;

  std::vector<NetLobbyMember> members;

};



// Compact per-frame input (client -> server). Foundation for server authority.

struct NetInput {

  std::uint32_t seq = 0;

  float move_x = 0.f;

  float move_z = 0.f;

  float yaw_deg = 0.f;

  float pitch_deg = 0.f;

  std::uint16_t buttons = 0;  // bit0 sprint,1 jump,2 crouch,3 fire,4 prone,5 boost

};



inline constexpr std::uint16_t kNetBtnSprint = 1 << 0;

inline constexpr std::uint16_t kNetBtnJump = 1 << 1;

inline constexpr std::uint16_t kNetBtnCrouch = 1 << 2;

inline constexpr std::uint16_t kNetBtnFire = 1 << 3;

inline constexpr std::uint16_t kNetBtnProne = 1 << 4;

inline constexpr std::uint16_t kNetBtnBoost = 1 << 5;



// One timestamped sample for snapshot interpolation.

struct NetPlayerSnapshot {

  double time = 0.0;

  float x = 0.f, y = 0.f, z = 0.f;

  float vx = 0.f, vy = 0.f, vz = 0.f;

  float yaw = 0.f, pitch = 0.f;

  std::uint8_t anim = 0;

  std::uint8_t flags = 0;

  std::int16_t health = 100;

  float anim_time = 0.f;

  std::uint8_t pose = 0;

  std::int16_t vehicle_id = -1;

  float veh_heading = 0.f, veh_pitch = 0.f, veh_roll = 0.f;

  float veh_rotor_rpm = 0.f, veh_rotor_spin = 0.f;

};



// Replicated per-player state. Kept flat and POD-friendly for cheap (de)serial.

struct NetPlayer {

  static constexpr int kHistorySize = 64;



  std::uint32_t id = 0;

  float x = 0.f, y = 0.f, z = 0.f;  // feet position (world); vehicle center when driving

  float yaw = 0.f, pitch = 0.f;     // look angles (degrees)

  std::uint8_t anim = 0;            // 0 idle, 1 walk, 2 run (legacy coarse bucket)

  std::uint8_t flags = 0;           // bit0: firing this tick

  std::int16_t health = 100;

  std::uint16_t faction_id = 0;

  bool active = false;



  // Extended replication (v2 wire tail).

  float vx = 0.f, vy = 0.f, vz = 0.f;

  float anim_time = 0.f;

  std::uint8_t pose = 0;

  std::int16_t vehicle_id = -1;

  float veh_heading = 0.f, veh_pitch = 0.f, veh_roll = 0.f;

  float veh_rotor_rpm = 0.f, veh_rotor_spin = 0.f;



  // v3: last processed input sequence (wire tail).

  std::uint32_t input_seq = 0;



  // Interpolated / dead-reckoned render pose (not serialized).

  float rx = 0.f, ry = 0.f, rz = 0.f, ryaw = 0.f, rpitch = 0.f;

  float rveh_heading = 0.f, rveh_pitch = 0.f, rveh_roll = 0.f;

  float rveh_rotor_rpm = 0.f, rveh_rotor_spin = 0.f;

  float ranim_time = 0.f;

  bool have_render = false;



  // Snapshot ring buffer for entity interpolation.

  NetPlayerSnapshot history[kHistorySize]{};

  int history_count = 0;

  int history_write = 0;

  double last_recv_time = 0.0;



  // Server-side last input from this player.

  NetInput last_input{};

};



struct NetShot {

  std::uint32_t shooter = 0;

  float ox = 0.f, oy = 0.f, oz = 0.f;

  float dx = 0.f, dy = 0.f, dz = 0.f;

};



class Net {

 public:

  ~Net();



  bool host(std::uint16_t port, bool dedicated, int max_clients = 16);

  bool connect(const std::string& address, std::uint16_t port);



  bool active() const { return role_ != Role::None; }

  bool is_server() const { return role_ == Role::Server; }

  bool is_client() const { return role_ == Role::Client; }

  bool dedicated() const { return dedicated_; }

  bool connected() const { return connected_; }

  std::uint32_t local_id() const { return local_id_; }

  double net_time() const { return net_time_; }



  // Receive packets + fixed-rate snapshot broadcast on the host.

  void poll(float dt);

  // Sample snapshot history and write rx/ry/rz (call just before render).

  void update_remotes();



  void send_local(const NetPlayer& me, const NetShot* shot);

  void send_input(const NetInput& input);



  const std::vector<NetPlayer>& players() const { return players_; }

  std::vector<NetShot> take_shots();

  int peer_count() const;



  bool in_lobby() const { return lobby_mode_; }

  void set_lobby_mode(bool on) { lobby_mode_ = on; }

  const NetLobbyState& lobby() const { return lobby_; }

  bool game_started() const { return lobby_.game_started; }

  bool lobby_joined() const { return lobby_joined_; }



  void send_join_info(const std::string& name, std::uint16_t faction_id);

  void set_ready(bool ready);

  void set_faction(std::uint16_t faction_id);

  void set_lobby_config(bool allow_late_join, const std::string& map_name,
                        const std::string& map_server_zip = {});

  void host_start_game();



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



  void ingest_player_state(NetPlayer& dst, const NetPlayer& src, double recv_time);

  void push_snapshot(NetPlayer& p, double time);

  void sample_interpolated(const NetPlayer& p, double render_time, NetPlayerSnapshot& out) const;



  Role role_ = Role::None;

  bool dedicated_ = false;

  bool connected_ = false;

  void* host_ = nullptr;

  void* peer_ = nullptr;

  std::uint32_t local_id_ = 0;

  std::uint32_t next_id_ = 1;

  std::vector<NetPlayer> players_;

  std::vector<NetShot> shots_;

  std::vector<NetShot> out_shots_;

  bool lobby_mode_ = false;

  bool lobby_joined_ = false;

  NetLobbyState lobby_;

  std::string local_name_;

  std::uint16_t local_faction_ = 0;

  bool local_ready_ = false;



  double net_time_ = 0.0;

  float snapshot_accum_ = 0.f;

  static constexpr float kSnapshotHz = 60.f;

  static constexpr float kInterpDelaySec = 0.1f;

  static constexpr float kSnapDistSq = 4.f;  // 2 m

  static constexpr float kMaxExtrapSec = 0.25f;

};



}  // namespace bf2

