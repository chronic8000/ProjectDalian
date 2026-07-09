#include "engine/net/net.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <enet/enet.h>

namespace bf2 {

namespace {

// ---- ENet library init refcount (init once across all Net instances) --------
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

// ---- Wire message tags ------------------------------------------------------
enum : std::uint8_t {
  MSG_WELCOME = 1,   // S->C: u32 id
  MSG_STATE = 2,     // C->S: one player record (the sender's own)
  MSG_SNAPSHOT = 3,  // S->C: u16 count, player records
  MSG_SHOT = 4,      // C->S: one shot record
  MSG_SHOTS = 5,     // S->C: u16 count, shot records
  MSG_JOIN = 6,      // C->S: u8 name_len, name, u16 faction
  MSG_LOBBY = 7,     // S->C: lobby blob
  MSG_READY = 8,     // C->S: u8 ready
  MSG_FACTION = 9,   // C->S: u16 faction
  MSG_START_GAME = 10,  // S->C or host->all
  MSG_LOBBY_CFG = 11,   // host->all: u8 allow_late, u8 map_len, map
};

// ---- Little-endian byte writer / reader ------------------------------------
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
    if (o + 1 > n) { ok = false; return 0; }
    return p[o++];
  }
  std::uint16_t u16() {
    if (o + 2 > n) { ok = false; return 0; }
    std::uint16_t v = std::uint16_t(p[o]) | (std::uint16_t(p[o + 1]) << 8);
    o += 2;
    return v;
  }
  std::uint32_t u32() {
    if (o + 4 > n) { ok = false; return 0; }
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
  pl.active = true;
  return pl;
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
  // Reuse a dead slot if one exists, else append.
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
  connected_ = true;  // a server is always "up"
  local_id_ = 0;      // the host player is id 0
  next_id_ = 1;
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
  connected_ = false;  // set on the CONNECT event
  lobby_mode_ = true;
  return true;
}

void Net::handle_packet(const std::uint8_t* data, std::size_t len, void* from_peer) {
  Reader r{data, len};
  const std::uint8_t tag = r.u8();
  if (role_ == Role::Server) {
    ENetPeer* peer = static_cast<ENetPeer*>(from_peer);
    const std::uint32_t pid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(peer->data));
    if (tag == MSG_STATE) {
      NetPlayer in = read_player(r);
      if (!r.ok) return;
      NetPlayer* s = slot(pid, true);
      if (s) {
        in.id = pid;  // trust the server-assigned id, not the client's claim
        in.active = true;
        // Preserve render-smoothing fields (server doesn't render remotes unless
        // it's a listen server, which smooths in poll()).
        in.rx = s->rx; in.ry = s->ry; in.rz = s->rz;
        in.ryaw = s->ryaw; in.rpitch = s->rpitch; in.have_render = s->have_render;
        *s = in;
      }
      if (NetLobbyMember* m = lobby_member(pid, false)) m->faction_id = in.faction_id;
    } else if (tag == MSG_SHOT) {
      NetShot sh = read_shot(r);
      if (!r.ok) return;
      sh.shooter = pid;
      shots_.push_back(sh);      // the host renders it
      out_shots_.push_back(sh);  // and relays it to the other clients
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
      // Mark all inactive, then re-activate those present in the snapshot.
      for (auto& p : players_) p.active = false;
      for (std::uint16_t i = 0; i < count && r.ok; ++i) {
        NetPlayer in = read_player(r);
        NetPlayer* s = slot(in.id, true);
        if (s) {
          in.rx = s->rx; in.ry = s->ry; in.rz = s->rz;
          in.ryaw = s->ryaw; in.rpitch = s->rpitch; in.have_render = s->have_render;
          *s = in;
        }
      }
    } else if (tag == MSG_SHOTS) {
      const std::uint16_t count = r.u16();
      for (std::uint16_t i = 0; i < count && r.ok; ++i) {
        NetShot sh = read_shot(r);
        if (sh.shooter != local_id_) shots_.push_back(sh);  // skip our own echo
      }
    }
  }
}

void Net::poll(float dt) {
  if (!host_) return;
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
        } else {
          connected_ = true;  // link established; await MSG_WELCOME for our id
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
          if (NetPlayer* s = slot(id, false)) s->active = false;
          for (auto it = lobby_.members.begin(); it != lobby_.members.end(); ++it) {
            if (it->id == id) {
              lobby_.members.erase(it);
              break;
            }
          }
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

  // Ease each remote's rendered transform toward its latest received state so
  // motion looks smooth between snapshots (basic interpolation/smoothing).
  const float k = dt > 0.f ? 1.f - std::exp(-14.f * dt) : 1.f;
  for (auto& p : players_) {
    if (!p.active) continue;
    if (!p.have_render) {
      p.rx = p.x; p.ry = p.y; p.rz = p.z; p.ryaw = p.yaw; p.rpitch = p.pitch;
      p.have_render = true;
      continue;
    }
    p.rx += (p.x - p.rx) * k;
    p.ry += (p.y - p.ry) * k;
    p.rz += (p.z - p.rz) * k;
    // Wrap-safe yaw easing.
    float dyaw = p.yaw - p.ryaw;
    while (dyaw > 180.f) dyaw -= 360.f;
    while (dyaw < -180.f) dyaw += 360.f;
    p.ryaw += dyaw * k;
    p.rpitch += (p.pitch - p.rpitch) * k;
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
  ENetPacket* pkt = enet_packet_create(snap.data(), snap.size(), 0);  // unreliable
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
        const bool hr = s->have_render;
        const float rx = s->rx, ry = s->ry, rz = s->rz, ryaw = s->ryaw, rpitch = s->rpitch;
        *s = me;
        s->id = 0;
        s->active = true;
        s->rx = rx; s->ry = ry; s->rz = rz; s->ryaw = ryaw; s->rpitch = rpitch;
        s->have_render = hr;
      }
    }
    if (shot) out_shots_.push_back(*shot);  // relay the host's own shot to clients
    broadcast_snapshot();
  } else if (role_ == Role::Client && connected_) {
    ENetPeer* p = static_cast<ENetPeer*>(peer_);
    std::vector<std::uint8_t> b;
    put_u8(b, MSG_STATE);
    NetPlayer m = me;
    m.id = local_id_;
    write_player(b, m);
    send_bytes(p, b, false);  // unreliable state stream
    if (shot) {
      std::vector<std::uint8_t> sb;
      put_u8(sb, MSG_SHOT);
      write_shot(sb, *shot);
      send_bytes(p, sb, true);  // reliable shot event
    }
    enet_host_flush(static_cast<ENetHost*>(host_));
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
}

}  // namespace bf2
