#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dalian {

inline std::uint16_t discovery_port(std::uint16_t game_port) {
  return static_cast<std::uint16_t>(game_port + 1);
}

struct ServerListing {
  std::string address;
  std::uint16_t game_port = 27015;
  std::string map_name;
  std::string host_name;
  int players = 0;
  bool in_game = false;
  bool allow_late_join = false;
};

// UDP LAN / Tailscale server browser.
class ServerBrowser {
 public:
  void begin_scan(std::uint16_t game_port, bool scan_lan, bool scan_tailscale,
                  const std::string& tailscale_subnet);
  void poll();
  bool scanning() const { return scanning_; }
  const std::vector<ServerListing>& servers() const { return servers_; }
  void clear();

 private:
  bool scanning_ = false;
  std::vector<ServerListing> servers_;
};

struct DiscoveryAdvert {
  std::uint16_t game_port = 27015;
  std::string map_name;
  std::string host_name;
  int players = 0;
  bool in_game = false;
  bool allow_late_join = true;
};

// Host-side UDP responder (runs while in lobby / in-game).
class DiscoveryHost {
 public:
  bool start(std::uint16_t game_port);
  void set_advert(const DiscoveryAdvert& advert);
  void set_broadcast_targets(bool lan, bool tailscale, const std::string& tailscale_subnet);
  void poll();
  void stop();

 private:
  void send_broadcast_announce();

  bool active_ = false;
  std::uint16_t game_port_ = 27015;
  DiscoveryAdvert advert_{};
  void* socket_ = nullptr;  // platform socket handle
  bool broadcast_lan_ = true;
  bool broadcast_tailscale_ = true;
  std::string tailscale_subnet_;
  float broadcast_timer_ = 0.f;
};

std::string detect_tailscale_subnet();
std::string detect_local_ip();

}  // namespace dalian
