#include "engine/net/net.hpp"



#include <cmath>

#include <cstdio>

#include <cstdlib>

#include <cstring>

#include <algorithm>



#include <enet/enet.h>



namespace bf2 {



namespace {



int g_enet_refs = 0;

bool enet_ref() {

  if (g_enet_refs == 0) {

    if (enet_initialize() != 0) return false;

  }

  ++g_enet_refs;

  return true;

}

void enet_unref() {

  if (g_enet_refs > 0 && --g_enet_refs == 0) enet_deinitialize();

}



enum : std::uint8_t {

  MSG_WELCOME = 1,

  MSG_STATE = 2,

  MSG_SNAPSHOT = 3,

  MSG_SHOT = 4,

  MSG_SHOTS = 5,

  MSG_JOIN = 6,

  MSG_LOBBY = 7,

  MSG_READY = 8,

  MSG_FACTION = 9,

  MSG_START_GAME = 10,

  MSG_LOBBY_CFG = 11,

  MSG_INPUT = 12,

};



constexpr std::size_t kPlayerV2TailBytes = 39;  // vx..veh_rotor_spin

constexpr std::size_t kPlayerV3ExtraBytes = 4;  // input_seq



void put_u8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }

void put_u16(std::vector<std::uint8_t>& b, std::uint16_t v) {

  b.push_back(std::uint8_t(v & 0xff));

  b.push_back(std::uint8_t((v >> 8) & 0xff));

}

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {

  for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t((v >> (8 * i)) & 0xff));

}

void put_f32(std::vector<std::uint8_t>& b, float f) {

  std::uint32_t u;

  std::memcpy(&u, &f, 4);

  put_u32(b, u);

}



struct Reader {

  const std::uint8_t* p;

  std::size_t n;

  std::size_t o = 0;

  bool ok = true;

  std::uint8_t u8() {

    if (o + 1 > n) {

      ok = false;

      return 0;

    }

    return p[o++];

  }

  std::uint16_t u16() {

    if (o + 2 > n) {

      ok = false;

      return 0;

    }

    std::uint16_t v = std::uint16_t(p[o]) | (std::uint16_t(p[o + 1]) << 8);

    o += 2;

    return v;

  }

  std::uint32_t u32() {

    if (o + 4 > n) {

      ok = false;

      return 0;

    }

    std::uint32_t v = 0;

    for (int i = 0; i < 4; ++i) v |= std::uint32_t(p[o + i]) << (8 * i);

    o += 4;

    return v;

  }

  float f32() {

    std::uint32_t u = u32();

    float f;

    std::memcpy(&f, &u, 4);

    return f;

  }

};



float lerp1(float a, float b, float t) { return a + (b - a) * t; }

float lerp_yaw_deg(float a, float b, float t) {

  float d = b - a;

  while (d > 180.f) d -= 360.f;

  while (d < -180.f) d += 360.f;

  return a + d * t;

}



NetPlayerSnapshot snapshot_from_player(const NetPlayer& p, double time) {

  NetPlayerSnapshot s;

  s.time = time;

  s.x = p.x;

  s.y = p.y;

  s.z = p.z;

  s.vx = p.vx;

  s.vy = p.vy;

  s.vz = p.vz;

  s.yaw = p.yaw;

  s.pitch = p.pitch;

  s.anim = p.anim;

  s.flags = p.flags;

  s.health = p.health;

  s.anim_time = p.anim_time;

  s.pose = p.pose;

  s.vehicle_id = p.vehicle_id;

  s.veh_heading = p.veh_heading;

  s.veh_pitch = p.veh_pitch;

  s.veh_roll = p.veh_roll;

  s.veh_rotor_rpm = p.veh_rotor_rpm;

  s.veh_rotor_spin = p.veh_rotor_spin;

  return s;

}



void write_player(std::vector<std::uint8_t>& b, const NetPlayer& pl) {

  put_u32(b, pl.id);

  put_f32(b, pl.x);

  put_f32(b, pl.y);

  put_f32(b, pl.z);

  put_f32(b, pl.yaw);

  put_f32(b, pl.pitch);

  put_u8(b, pl.anim);

  put_u8(b, pl.flags);

  put_u16(b, std::uint16_t(pl.health));

  put_u16(b, pl.faction_id);

  put_f32(b, pl.vx);

  put_f32(b, pl.vy);

  put_f32(b, pl.vz);

  put_f32(b, pl.anim_time);

  put_u8(b, pl.pose);

  if (pl.vehicle_id < 0)

    put_u16(b, 0xffff);

  else

    put_u16(b, static_cast<std::uint16_t>(pl.vehicle_id));

  put_f32(b, pl.veh_heading);

  put_f32(b, pl.veh_pitch);

  put_f32(b, pl.veh_roll);

  put_f32(b, pl.veh_rotor_rpm);

  put_f32(b, pl.veh_rotor_spin);

  put_u32(b, pl.input_seq);

}



NetPlayer read_player(Reader& r) {

  NetPlayer pl;

  pl.id = r.u32();

  pl.x = r.f32();

  pl.y = r.f32();

  pl.z = r.f32();

  pl.yaw = r.f32();

  pl.pitch = r.f32();

  pl.anim = r.u8();

  pl.flags = r.u8();

  pl.health = std::int16_t(r.u16());

  if (r.o < r.n) pl.faction_id = r.u16();

  if (r.o + kPlayerV2TailBytes <= r.n) {

    pl.vx = r.f32();

    pl.vy = r.f32();

    pl.vz = r.f32();

    pl.anim_time = r.f32();

    pl.pose = r.u8();

    const std::uint16_t vid = r.u16();

    pl.vehicle_id = vid == 0xffff ? std::int16_t(-1) : static_cast<std::int16_t>(vid);

    pl.veh_heading = r.f32();

    pl.veh_pitch = r.f32();

    pl.veh_roll = r.f32();

    pl.veh_rotor_rpm = r.f32();

    pl.veh_rotor_spin = r.f32();

  }

  if (r.o + kPlayerV3ExtraBytes <= r.n) pl.input_seq = r.u32();

  pl.active = true;

  return pl;

}



void write_input(std::vector<std::uint8_t>& b, const NetInput& in) {

  put_u32(b, in.seq);

  put_f32(b, in.move_x);

  put_f32(b, in.move_z);

  put_f32(b, in.yaw_deg);

  put_f32(b, in.pitch_deg);

  put_u16(b, in.buttons);

}



NetInput read_input(Reader& r) {

  NetInput in;

  in.seq = r.u32();

  in.move_x = r.f32();

  in.move_z = r.f32();

  in.yaw_deg = r.f32();

  in.pitch_deg = r.f32();

  in.buttons = r.u16();

  return in;

}



void write_shot(std::vector<std::uint8_t>& b, const NetShot& s) {

  put_u32(b, s.shooter);

  put_f32(b, s.ox);

  put_f32(b, s.oy);

  put_f32(b, s.oz);

  put_f32(b, s.dx);

  put_f32(b, s.dy);

  put_f32(b, s.dz);

}

NetShot read_shot(Reader& r) {

  NetShot s;

  s.shooter = r.u32();

  s.ox = r.f32();

  s.oy = r.f32();

  s.oz = r.f32();

  s.dx = r.f32();

  s.dy = r.f32();

  s.dz = r.f32();

  return s;

}



void send_bytes(ENetPeer* peer, const std::vector<std::uint8_t>& b, bool reliable) {

  if (!peer) return;

  ENetPacket* pkt = enet_packet_create(b.data(), b.size(),

                                       reliable ? ENET_PACKET_FLAG_RELIABLE : 0);

  enet_peer_send(peer, reliable ? 0 : 1, pkt);

}



void put_str(std::vector<std::uint8_t>& b, const std::string& s, int max_len) {

  const int n = std::min(static_cast<int>(s.size()), max_len);

  put_u8(b, static_cast<std::uint8_t>(n));

  for (int i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(s[i]));

}

std::string read_str(Reader& r, int max_len) {

  const std::uint8_t n = r.u8();

  if (!r.ok || n > max_len) return {};

  std::string out;

  out.reserve(n);

  for (std::uint8_t i = 0; i < n && r.ok; ++i) out.push_back(static_cast<char>(r.u8()));

  return out;

}



void write_lobby(std::vector<std::uint8_t>& b, const NetLobbyState& lob) {

  put_u8(b, lob.allow_late_join ? 1 : 0);

  put_u8(b, lob.game_started ? 1 : 0);

  put_str(b, lob.map_name, 200);

  put_u16(b, static_cast<std::uint16_t>(lob.members.size()));

  for (const auto& m : lob.members) {

    put_u32(b, m.id);

    put_str(b, m.name, 48);

    put_u16(b, m.faction_id);

    put_u8(b, m.ready ? 1 : 0);

    put_u8(b, m.is_host ? 1 : 0);

  }

}

NetLobbyState read_lobby(Reader& r) {

  NetLobbyState lob;

  lob.allow_late_join = r.u8() != 0;

  lob.game_started = r.u8() != 0;

  lob.map_name = read_str(r, 200);

  const std::uint16_t count = r.u16();

  for (std::uint16_t i = 0; i < count && r.ok; ++i) {

    NetLobbyMember m;

    m.id = r.u32();

    m.name = read_str(r, 48);

    m.faction_id = r.u16();

    m.ready = r.u8() != 0;

    m.is_host = r.u8() != 0;

    lob.members.push_back(m);

  }

  return lob;

}



}  // namespace



Net::~Net() { shutdown(); }



void Net::push_snapshot(NetPlayer& p, double time) {

  NetPlayerSnapshot snap = snapshot_from_player(p, time);

  if (p.history_count > 0) {

    const NetPlayerSnapshot& prev =

        p.history[(p.history_write - 1 + NetPlayer::kHistorySize) % NetPlayer::kHistorySize];

    const float dt = static_cast<float>(time - prev.time);

    if (dt > 0.0001f) {

      const float dx = snap.x - prev.x;

      const float dy = snap.y - prev.y;

      const float dz = snap.z - prev.z;

      const float hspd = std::sqrt(dx * dx + dz * dz);

      if (hspd > 0.05f && std::sqrt(snap.vx * snap.vx + snap.vz * snap.vz) < 0.15f) {

        snap.vx = dx / dt;

        snap.vy = dy / dt;

        snap.vz = dz / dt;

      }

    }

  }

  p.history[p.history_write] = snap;

  p.history_write = (p.history_write + 1) % NetPlayer::kHistorySize;

  if (p.history_count < NetPlayer::kHistorySize) ++p.history_count;

}



void Net::ingest_player_state(NetPlayer& dst, const NetPlayer& src, double recv_time) {

  const int hc = dst.history_count;

  const int hw = dst.history_write;

  const double lrt = dst.last_recv_time;

  const bool hr = dst.have_render;

  const float rx = dst.rx, ry = dst.ry, rz = dst.rz;

  const float ryaw = dst.ryaw, rpitch = dst.rpitch;

  const float rvh = dst.rveh_heading, rvp = dst.rveh_pitch, rvr = dst.rveh_roll;

  const float rvrpm = dst.rveh_rotor_rpm, rvspin = dst.rveh_rotor_spin;

  const float rat = dst.ranim_time;

  const NetInput li = dst.last_input;



  if (hc > 0 && recv_time > lrt + 0.0001) {
    const NetPlayerSnapshot& prev =
        dst.history[(hw - 1 + NetPlayer::kHistorySize) % NetPlayer::kHistorySize];
    const float dt = static_cast<float>(recv_time - prev.time);
    if (dt > 0.0001f) {
      const float dx = src.x - prev.x;
      const float dy = src.y - prev.y;
      const float dz = src.z - prev.z;
      if (std::sqrt(dx * dx + dz * dz) > 0.05f &&
          std::sqrt(src.vx * src.vx + src.vz * src.vz) < 0.15f) {
        NetPlayer patched = src;
        patched.vx = dx / dt;
        patched.vy = dy / dt;
        patched.vz = dz / dt;
        dst = patched;
        dst.history_count = hc;
        dst.history_write = hw;
        dst.last_recv_time = lrt;
        dst.have_render = hr;
        dst.rx = rx;
        dst.ry = ry;
        dst.rz = rz;
        dst.ryaw = ryaw;
        dst.rpitch = rpitch;
        dst.rveh_heading = rvh;
        dst.rveh_pitch = rvp;
        dst.rveh_roll = rvr;
        dst.rveh_rotor_rpm = rvrpm;
        dst.rveh_rotor_spin = rvspin;
        dst.ranim_time = rat;
        dst.last_input = li;
        dst.last_recv_time = recv_time;
        push_snapshot(dst, recv_time);
        return;
      }
    }
  }

  dst = src;

  dst.history_count = hc;

  dst.history_write = hw;

  dst.last_recv_time = recv_time;

  dst.have_render = hr;

  dst.rx = rx;

  dst.ry = ry;

  dst.rz = rz;

  dst.ryaw = ryaw;

  dst.rpitch = rpitch;

  dst.rveh_heading = rvh;

  dst.rveh_pitch = rvp;

  dst.rveh_roll = rvr;

  dst.rveh_rotor_rpm = rvrpm;

  dst.rveh_rotor_spin = rvspin;

  dst.ranim_time = rat;

  dst.last_input = li;

  push_snapshot(dst, recv_time);
}



void Net::sample_interpolated(const NetPlayer& p, double render_time, NetPlayerSnapshot& out) const {

  if (p.history_count == 0) {

    out = snapshot_from_player(p, render_time);

    return;

  }

  if (p.history_count == 1) {

    out = p.history[(p.history_write - 1 + NetPlayer::kHistorySize) % NetPlayer::kHistorySize];

    const float age = static_cast<float>(render_time - out.time);

    if (age > 0.f) {

      const float lead = std::min(age, kMaxExtrapSec);

      out.x += out.vx * lead;

      out.y += out.vy * lead;

      out.z += out.vz * lead;

      out.anim_time += lead;

    }

    return;

  }



  const int newest_i = (p.history_write - 1 + NetPlayer::kHistorySize) % NetPlayer::kHistorySize;

  const NetPlayerSnapshot& newest = p.history[newest_i];



  int older_i = -1;

  int newer_i = -1;

  for (int n = 0; n < p.history_count; ++n) {

    const int idx = (p.history_write - 1 - n + NetPlayer::kHistorySize * 2) % NetPlayer::kHistorySize;

    const NetPlayerSnapshot& s = p.history[idx];

    if (s.time <= render_time) {

      older_i = idx;

      break;

    }

    newer_i = idx;

  }



  if (older_i < 0) {

    out = newest;

    const float age = static_cast<float>(render_time - out.time);

    const float lead = std::clamp(age, 0.f, kMaxExtrapSec);

    out.x += out.vx * lead;

    out.y += out.vy * lead;

    out.z += out.vz * lead;

    out.anim_time += lead;

    return;

  }



  const NetPlayerSnapshot& older = p.history[older_i];

  if (newer_i < 0 || newer_i == older_i) {

    out = older;

    const float age = static_cast<float>(render_time - out.time);

    const float lead = std::clamp(age, 0.f, kMaxExtrapSec);

    out.x += out.vx * lead;

    out.y += out.vy * lead;

    out.z += out.vz * lead;

    out.anim_time += lead;

    return;

  }



  const NetPlayerSnapshot& newer = p.history[newer_i];

  const double span = newer.time - older.time;

  const float t = span > 0.0001 ? static_cast<float>((render_time - older.time) / span) : 1.f;

  const float u = std::clamp(t, 0.f, 1.f);



  out.time = render_time;

  out.x = lerp1(older.x, newer.x, u);

  out.y = lerp1(older.y, newer.y, u);

  out.z = lerp1(older.z, newer.z, u);

  out.vx = lerp1(older.vx, newer.vx, u);

  out.vy = lerp1(older.vy, newer.vy, u);

  out.vz = lerp1(older.vz, newer.vz, u);

  out.yaw = lerp_yaw_deg(older.yaw, newer.yaw, u);

  out.pitch = lerp1(older.pitch, newer.pitch, u);

  out.anim = newer.anim;

  out.flags = newer.flags;

  out.health = newer.health;

  out.anim_time = lerp1(older.anim_time, newer.anim_time, u);

  out.pose = newer.pose;

  out.vehicle_id = newer.vehicle_id;

  out.veh_heading = lerp_yaw_deg(older.veh_heading, newer.veh_heading, u);

  out.veh_pitch = lerp1(older.veh_pitch, newer.veh_pitch, u);

  out.veh_roll = lerp1(older.veh_roll, newer.veh_roll, u);

  out.veh_rotor_rpm = lerp1(older.veh_rotor_rpm, newer.veh_rotor_rpm, u);

  out.veh_rotor_spin = lerp1(older.veh_rotor_spin, newer.veh_rotor_spin, u);

}



void Net::update_remotes() {

  const double render_time = net_time_ - kInterpDelaySec;

  for (auto& p : players_) {

    if (!p.active) continue;

    if (p.id == local_id_) continue;



    NetPlayerSnapshot view;

    sample_interpolated(p, render_time, view);



    const float dx = view.x - p.x;

    const float dy = view.y - p.y;

    const float dz = view.z - p.z;

    if (dx * dx + dy * dy + dz * dz > kSnapDistSq) {

      view.x = p.x;

      view.y = p.y;

      view.z = p.z;

      view.vx = p.vx;

      view.vy = p.vy;

      view.vz = p.vz;

      view.yaw = p.yaw;

      view.pitch = p.pitch;

      view.anim_time = p.anim_time;

    }



    if (!p.have_render) {

      p.rx = view.x;

      p.ry = view.y;

      p.rz = view.z;

      p.ryaw = view.yaw;

      p.rpitch = view.pitch;

      p.rveh_heading = view.veh_heading;

      p.rveh_pitch = view.veh_pitch;

      p.rveh_roll = view.veh_roll;

      p.rveh_rotor_rpm = view.veh_rotor_rpm;

      p.rveh_rotor_spin = view.veh_rotor_spin;

      p.ranim_time = view.anim_time;

      p.have_render = true;

      continue;

    }



    p.rx = view.x;

    p.ry = view.y;

    p.rz = view.z;

    p.ryaw = view.yaw;

    p.rpitch = view.pitch;

    p.rveh_heading = view.veh_heading;

    p.rveh_pitch = view.veh_pitch;

    p.rveh_roll = view.veh_roll;

    p.rveh_rotor_rpm = view.veh_rotor_rpm;

    p.rveh_rotor_spin = view.veh_rotor_spin;

    p.ranim_time = view.anim_time;

  }

}



NetLobbyMember* Net::lobby_member(std::uint32_t id, bool create) {

  for (auto& m : lobby_.members) {

    if (m.id == id) return &m;

  }

  if (!create) return nullptr;

  NetLobbyMember m;

  m.id = id;

  lobby_.members.push_back(m);

  return &lobby_.members.back();

}



void Net::send_lobby_to(void* peer) {

  if (role_ != Role::Server || !peer) return;

  std::vector<std::uint8_t> b;

  put_u8(b, MSG_LOBBY);

  write_lobby(b, lobby_);

  send_bytes(static_cast<ENetPeer*>(peer), b, true);

}



void Net::broadcast_lobby() {

  if (role_ != Role::Server || !host_) return;

  ENetHost* h = static_cast<ENetHost*>(host_);

  std::vector<std::uint8_t> b;

  put_u8(b, MSG_LOBBY);

  write_lobby(b, lobby_);

  ENetPacket* pkt = enet_packet_create(b.data(), b.size(), ENET_PACKET_FLAG_RELIABLE);

  enet_host_broadcast(h, 0, pkt);

}



void Net::send_join_info(const std::string& name, std::uint16_t faction_id) {

  local_name_ = name.substr(0, 48);

  local_faction_ = faction_id;

  if (role_ == Role::Server) {

    if (NetLobbyMember* m = lobby_member(0, true)) {

      m->name = local_name_;

      m->faction_id = faction_id;

      m->is_host = true;

      m->ready = local_ready_;

    }

    lobby_joined_ = true;

    broadcast_lobby();

    return;

  }

  if (role_ == Role::Client && connected_) {

    std::vector<std::uint8_t> b;

    put_u8(b, MSG_JOIN);

    put_str(b, local_name_, 48);

    put_u16(b, faction_id);

    send_bytes(static_cast<ENetPeer*>(peer_), b, true);

    enet_host_flush(static_cast<ENetHost*>(host_));

  }

}



void Net::set_ready(bool ready) {

  local_ready_ = ready;

  if (role_ == Role::Server) {

    if (NetLobbyMember* m = lobby_member(0, true)) m->ready = ready;

    broadcast_lobby();

    return;

  }

  if (role_ == Role::Client && connected_) {

    std::vector<std::uint8_t> b;

    put_u8(b, MSG_READY);

    put_u8(b, ready ? 1 : 0);

    send_bytes(static_cast<ENetPeer*>(peer_), b, true);

    enet_host_flush(static_cast<ENetHost*>(host_));

  }

}



void Net::set_faction(std::uint16_t faction_id) {

  local_faction_ = faction_id;

  if (role_ == Role::Server) {

    if (NetLobbyMember* m = lobby_member(0, true)) m->faction_id = faction_id;

    if (NetPlayer* p = slot(0, false)) p->faction_id = faction_id;

    if (lobby_mode_) broadcast_lobby();

    return;

  }

  if (role_ == Role::Client && connected_) {

    if (NetPlayer* p = slot(local_id_, false)) p->faction_id = faction_id;

    std::vector<std::uint8_t> b;

    put_u8(b, MSG_FACTION);

    put_u16(b, faction_id);

    send_bytes(static_cast<ENetPeer*>(peer_), b, true);

    enet_host_flush(static_cast<ENetHost*>(host_));

  }

}



std::string Net::lobby_player_name(std::uint32_t id) const {

  for (const auto& m : lobby_.members) {

    if (m.id == id) return m.name;

  }

  return {};

}



std::uint16_t Net::lobby_player_faction(std::uint32_t id) const {

  for (const auto& m : lobby_.members) {

    if (m.id == id) return m.faction_id;

  }

  for (const auto& p : players_) {

    if (p.id == id && p.active) return p.faction_id;

  }

  return 0;

}



void Net::set_lobby_config(bool allow_late_join, const std::string& map_name) {

  if (role_ != Role::Server) return;

  lobby_.allow_late_join = allow_late_join;

  lobby_.map_name = map_name.substr(0, 200);

  broadcast_lobby();

}



void Net::host_start_game() {

  if (role_ != Role::Server) return;

  lobby_.game_started = true;

  lobby_mode_ = false;

  broadcast_lobby();

  std::vector<std::uint8_t> b;

  put_u8(b, MSG_START_GAME);

  ENetHost* h = static_cast<ENetHost*>(host_);

  ENetPacket* pkt = enet_packet_create(b.data(), b.size(), ENET_PACKET_FLAG_RELIABLE);

  enet_host_broadcast(h, 0, pkt);

}



NetPlayer* Net::slot(std::uint32_t id, bool create) {

  for (auto& p : players_) {

    if (p.id == id && p.active) return &p;

  }

  if (!create) return nullptr;

  for (auto& p : players_) {

    if (!p.active) {

      p = NetPlayer{};

      p.id = id;

      p.active = true;

      return &p;

    }

  }

  NetPlayer p;

  p.id = id;

  p.active = true;

  players_.push_back(p);

  return &players_.back();

}



bool Net::host(std::uint16_t port, bool dedicated, int max_clients) {

  if (!enet_ref()) return false;

  ENetAddress addr;

  addr.host = ENET_HOST_ANY;

  addr.port = port;

  ENetHost* h = enet_host_create(&addr, static_cast<std::size_t>(max_clients), 2, 0, 0);

  if (!h) {

    enet_unref();

    return false;

  }

  host_ = h;

  role_ = Role::Server;

  dedicated_ = dedicated;

  connected_ = true;

  local_id_ = 0;

  next_id_ = 1;

  net_time_ = 0.0;

  snapshot_accum_ = 0.f;

  if (!dedicated) {

    slot(0, true);

    if (NetPlayer* p = slot(0, true)) p->faction_id = local_faction_;

  }

  lobby_mode_ = true;

  lobby_.members.clear();

  lobby_.game_started = false;

  return true;

}



bool Net::connect(const std::string& address, std::uint16_t port) {

  if (!enet_ref()) return false;

  ENetHost* h = enet_host_create(nullptr, 1, 2, 0, 0);

  if (!h) {

    enet_unref();

    return false;

  }

  ENetAddress addr;

  if (enet_address_set_host(&addr, address.c_str()) != 0) {

    enet_host_destroy(h);

    enet_unref();

    return false;

  }

  addr.port = port;

  ENetPeer* p = enet_host_connect(h, &addr, 2, 0);

  if (!p) {

    enet_host_destroy(h);

    enet_unref();

    return false;

  }

  host_ = h;

  peer_ = p;

  role_ = Role::Client;

  connected_ = false;

  lobby_mode_ = true;

  net_time_ = 0.0;

  snapshot_accum_ = 0.f;

  return true;

}



void Net::handle_packet(const std::uint8_t* data, std::size_t len, void* from_peer) {

  Reader r{data, len};

  const std::uint8_t tag = r.u8();

  const double recv_time = net_time_;

  if (role_ == Role::Server) {

    ENetPeer* peer = static_cast<ENetPeer*>(from_peer);

    const std::uint32_t pid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(peer->data));

    if (tag == MSG_STATE) {

      NetPlayer in = read_player(r);

      if (!r.ok) return;

      NetPlayer* s = slot(pid, true);

      if (s) {

        in.id = pid;

        in.active = true;

        ingest_player_state(*s, in, recv_time);

      }

      if (NetLobbyMember* m = lobby_member(pid, false)) m->faction_id = in.faction_id;

      broadcast_snapshot();

      enet_host_flush(static_cast<ENetHost*>(host_));

    } else if (tag == MSG_INPUT) {

      NetInput in = read_input(r);

      if (!r.ok) return;

      if (NetPlayer* s = slot(pid, false)) {

        s->last_input = in;

        s->input_seq = in.seq;

      }

      broadcast_snapshot();

      enet_host_flush(static_cast<ENetHost*>(host_));

    } else if (tag == MSG_SHOT) {

      NetShot sh = read_shot(r);

      if (!r.ok) return;

      sh.shooter = pid;

      shots_.push_back(sh);

      out_shots_.push_back(sh);

    } else if (tag == MSG_JOIN) {

      const std::string name = read_str(r, 48);

      const std::uint16_t faction = r.u16();

      if (!r.ok) return;

      if (lobby_.game_started && !lobby_.allow_late_join) return;

      if (NetLobbyMember* m = lobby_member(pid, true)) {

        m->name = name;

        m->faction_id = faction;

        m->ready = false;

        m->is_host = false;

      }

      if (NetPlayer* p = slot(pid, true)) p->faction_id = faction;

      lobby_joined_ = true;

      broadcast_lobby();

      if (lobby_.game_started) {

        std::vector<std::uint8_t> b;

        put_u8(b, MSG_START_GAME);

        send_bytes(peer, b, true);

      }

    } else if (tag == MSG_READY) {

      const bool ready = r.u8() != 0;

      if (!r.ok) return;

      if (NetLobbyMember* m = lobby_member(pid, false)) {

        m->ready = ready;

        broadcast_lobby();

      }

    } else if (tag == MSG_FACTION) {

      const std::uint16_t faction = r.u16();

      if (!r.ok) return;

      if (NetLobbyMember* m = lobby_member(pid, false)) m->faction_id = faction;

      if (NetPlayer* p = slot(pid, false)) p->faction_id = faction;

      if (lobby_mode_) broadcast_lobby();

    }

  } else if (role_ == Role::Client) {

    if (tag == MSG_WELCOME) {

      local_id_ = r.u32();

      connected_ = true;

      if (!local_name_.empty()) send_join_info(local_name_, local_faction_);

      std::printf("Net: welcomed as player id=%u\n", local_id_);

      if (std::getenv("BF2_NETDEBUG"))

        std::fprintf(stderr, "[net] client: welcomed as id=%u\n", local_id_);

    } else if (tag == MSG_LOBBY) {

      lobby_ = read_lobby(r);

      lobby_joined_ = true;

    } else if (tag == MSG_START_GAME) {

      lobby_.game_started = true;

      lobby_mode_ = false;

    } else if (tag == MSG_SNAPSHOT) {

      const std::uint16_t count = r.u16();

      for (auto& p : players_) p.active = false;

      for (std::uint16_t i = 0; i < count && r.ok; ++i) {

        NetPlayer in = read_player(r);

        NetPlayer* s = slot(in.id, true);

        if (s) ingest_player_state(*s, in, recv_time);

      }

    } else if (tag == MSG_SHOTS) {

      const std::uint16_t count = r.u16();

      for (std::uint16_t i = 0; i < count && r.ok; ++i) {

        NetShot sh = read_shot(r);

        if (sh.shooter != local_id_) shots_.push_back(sh);

      }

    }

  }

}



void Net::poll(float dt) {

  if (!host_) return;

  if (dt > 0.f) net_time_ += dt;



  ENetHost* h = static_cast<ENetHost*>(host_);

  ENetEvent ev;

  while (enet_host_service(h, &ev, 0) > 0) {

    switch (ev.type) {

      case ENET_EVENT_TYPE_CONNECT: {

        if (role_ == Role::Server) {

          const std::uint32_t id = next_id_++;

          ev.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));

          slot(id, true);

          std::vector<std::uint8_t> b;

          put_u8(b, MSG_WELCOME);

          put_u32(b, id);

          send_bytes(ev.peer, b, true);

          send_lobby_to(ev.peer);

          if (lobby_.game_started) {

            std::vector<std::uint8_t> sb;

            put_u8(sb, MSG_START_GAME);

            send_bytes(ev.peer, sb, true);

          }

          if (std::getenv("BF2_NETDEBUG"))

            std::fprintf(stderr, "[net] server: client connected, assigned id=%u\n", id);

          std::printf("Net: player joined (id=%u, %d connected)\n", id,

                      static_cast<int>(lobby_.members.size()));

        } else {

          connected_ = true;

          std::printf("Net: connected to server, waiting for welcome...\n");

          if (std::getenv("BF2_NETDEBUG")) std::fprintf(stderr, "[net] client: link established\n");

        }

        break;

      }

      case ENET_EVENT_TYPE_RECEIVE:

        handle_packet(ev.packet->data, ev.packet->dataLength, ev.peer);

        enet_packet_destroy(ev.packet);

        break;

      case ENET_EVENT_TYPE_DISCONNECT: {

        if (role_ == Role::Server) {

          const std::uint32_t id =

              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(ev.peer->data));

          std::string left_name;

          for (const auto& m : lobby_.members) {

            if (m.id == id) {

              left_name = m.name;

              break;

            }

          }

          if (NetPlayer* s = slot(id, false)) s->active = false;

          for (auto it = lobby_.members.begin(); it != lobby_.members.end(); ++it) {

            if (it->id == id) {

              if (left_name.empty()) left_name = it->name;

              lobby_.members.erase(it);

              break;

            }

          }

          if (!left_name.empty())

            std::printf("Net: player left (%s, id=%u)\n", left_name.c_str(), id);

          else

            std::printf("Net: player left (id=%u)\n", id);

          broadcast_lobby();

          ev.peer->data = nullptr;

        } else {

          connected_ = false;

        }

        break;

      }

      default:

        break;

    }

  }



  if (role_ == Role::Server && dt > 0.f) {

    snapshot_accum_ += dt;

    const float step = 1.f / kSnapshotHz;

    while (snapshot_accum_ >= step) {

      snapshot_accum_ -= step;

      broadcast_snapshot();

    }

    enet_host_flush(h);

  }

}



void Net::broadcast_snapshot() {

  if (role_ != Role::Server || !host_) return;

  ENetHost* h = static_cast<ENetHost*>(host_);

  std::vector<std::uint8_t> snap;

  put_u8(snap, MSG_SNAPSHOT);

  std::uint16_t count = 0;

  std::vector<std::uint8_t> body;

  for (const auto& p : players_) {

    if (!p.active) continue;

    write_player(body, p);

    ++count;

  }

  put_u16(snap, count);

  snap.insert(snap.end(), body.begin(), body.end());

  ENetPacket* pkt = enet_packet_create(snap.data(), snap.size(), 0);

  enet_host_broadcast(h, 1, pkt);



  if (!out_shots_.empty()) {

    std::vector<std::uint8_t> sb;

    put_u8(sb, MSG_SHOTS);

    put_u16(sb, std::uint16_t(out_shots_.size()));

    for (const auto& s : out_shots_) write_shot(sb, s);

    ENetPacket* sp = enet_packet_create(sb.data(), sb.size(), ENET_PACKET_FLAG_RELIABLE);

    enet_host_broadcast(h, 0, sp);

    out_shots_.clear();

  }

}



void Net::send_local(const NetPlayer& me, const NetShot* shot) {

  if (role_ == Role::Server) {

    if (!dedicated_) {

      NetPlayer* s = slot(0, true);

      if (s) {

        NetPlayer in = me;

        in.id = 0;

        in.active = true;

        ingest_player_state(*s, in, net_time_);

      }

    }

    if (shot) out_shots_.push_back(*shot);

    broadcast_snapshot();

    enet_host_flush(static_cast<ENetHost*>(host_));

  } else if (role_ == Role::Client && connected_) {

    ENetPeer* p = static_cast<ENetPeer*>(peer_);

    std::vector<std::uint8_t> b;

    put_u8(b, MSG_STATE);

    NetPlayer m = me;

    m.id = local_id_;

    write_player(b, m);

    send_bytes(p, b, false);

    if (shot) {

      std::vector<std::uint8_t> sb;

      put_u8(sb, MSG_SHOT);

      write_shot(sb, *shot);

      send_bytes(p, sb, true);

    }

    enet_host_flush(static_cast<ENetHost*>(host_));

  }

}



void Net::send_input(const NetInput& input) {

  if (role_ == Role::Client && connected_) {

    std::vector<std::uint8_t> b;

    put_u8(b, MSG_INPUT);

    write_input(b, input);

    send_bytes(static_cast<ENetPeer*>(peer_), b, false);

    enet_host_flush(static_cast<ENetHost*>(host_));

  } else if (role_ == Role::Server && !dedicated_) {

    if (NetPlayer* s = slot(0, false)) {

      s->last_input = input;

      s->input_seq = input.seq;

    }

  }

}



std::vector<NetShot> Net::take_shots() {

  std::vector<NetShot> out;

  out.swap(shots_);

  return out;

}



int Net::peer_count() const {

  int n = 0;

  for (const auto& p : players_) {

    if (p.active && p.id != local_id_) ++n;

  }

  return n;

}



void Net::shutdown() {

  if (host_) {

    ENetHost* h = static_cast<ENetHost*>(host_);

    if (role_ == Role::Client && peer_) {

      enet_peer_disconnect_now(static_cast<ENetPeer*>(peer_), 0);

    }

    enet_host_flush(h);

    enet_host_destroy(h);

    enet_unref();

  }

  host_ = nullptr;

  peer_ = nullptr;

  role_ = Role::None;

  connected_ = false;

  players_.clear();

  shots_.clear();

  out_shots_.clear();

  lobby_.members.clear();

  lobby_.game_started = false;

  lobby_joined_ = false;

  lobby_mode_ = false;

  net_time_ = 0.0;

  snapshot_accum_ = 0.f;

}



}  // namespace bf2

