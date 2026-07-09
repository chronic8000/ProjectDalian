#include "server_discovery.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace dalian {
namespace {

constexpr char kQuery[] = {'P', 'D', 'Q', '\0'};
constexpr char kReply[] = {'P', 'D', 'R', '\0'};

struct WsaInit {
  WsaInit() {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  }
  ~WsaInit() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};

WsaInit& wsa() {
  static WsaInit init;
  return init;
}

void close_socket(SocketHandle s) {
  if (s == kInvalidSocket) return;
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

bool set_nonblock(SocketHandle s) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(s, F_GETFL, 0);
  return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool send_query(SocketHandle s, const sockaddr_in& addr) {
  return sendto(s, kQuery, 4, 0, reinterpret_cast<const sockaddr*>(&addr),
                sizeof(addr)) == 4;
}

bool parse_reply(const char* buf, int len, ServerListing& out) {
  if (len < 12 || std::memcmp(buf, kReply, 4) != 0) return false;
  out.game_port = static_cast<std::uint16_t>(static_cast<unsigned char>(buf[4]) |
                                               (static_cast<unsigned char>(buf[5]) << 8));
  out.players = static_cast<unsigned char>(buf[6]);
  out.in_game = buf[7] != 0;
  out.allow_late_join = buf[8] != 0;
  const int map_len = static_cast<unsigned char>(buf[9]);
  if (len < 10 + map_len + 1) return false;
  out.map_name.assign(buf + 10, map_len);
  const int host_len = static_cast<unsigned char>(buf[10 + map_len]);
  if (len < 11 + map_len + host_len) return false;
  out.host_name.assign(buf + 11 + map_len, host_len);
  return true;
}

std::vector<std::uint32_t> build_scan_targets(bool scan_lan, bool scan_tailscale,
                                              const std::string& tailscale_subnet) {
  std::vector<std::uint32_t> ips;
  if (scan_lan) ips.push_back(0xFFFFFFFFu);
  if (scan_tailscale && !tailscale_subnet.empty()) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(tailscale_subnet.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
      const std::uint32_t base = (a << 24) | (b << 16) | (c << 8);
      for (int i = 1; i < 255; ++i) ips.push_back(base | static_cast<std::uint32_t>(i));
    }
  }
  return ips;
}

}  // namespace

std::string detect_tailscale_subnet() {
  FILE* fp = _popen("tailscale ip -4 2>nul", "r");
  if (!fp) return {};
  char line[64] = {};
  if (!std::fgets(line, sizeof(line), fp)) {
    _pclose(fp);
    return {};
  }
  _pclose(fp);
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (std::sscanf(line, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return {};
  char subnet[32];
  std::snprintf(subnet, sizeof(subnet), "%u.%u.%u.0", a, b, c);
  return subnet;
}

std::string detect_local_ip() {
  const std::string ts = detect_tailscale_subnet();
  if (!ts.empty()) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ts.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
      char ip[32];
      std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, d);
      return ip;
    }
  }
#ifdef _WIN32
  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
      char ip[INET_ADDRSTRLEN] = {};
      auto* in = reinterpret_cast<sockaddr_in*>(res->ai_addr);
      inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
      freeaddrinfo(res);
      if (ip[0] != '\0' && std::strcmp(ip, "127.0.0.1") != 0) return ip;
    }
  }
#endif
  return "127.0.0.1";
}

void ServerBrowser::clear() {
  scanning_ = false;
  servers_.clear();
}

void ServerBrowser::begin_scan(std::uint16_t game_port, bool scan_lan, bool scan_tailscale,
                               const std::string& tailscale_subnet) {
  wsa();
  clear();
  scanning_ = true;
  const std::uint16_t dport = discovery_port(game_port);
  const auto targets = build_scan_targets(scan_lan, scan_tailscale, tailscale_subnet);
  std::thread([this, dport, targets]() {
    SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) {
      scanning_ = false;
      return;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    set_nonblock(s);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dport);
    for (std::uint32_t ip : targets) {
      dst.sin_addr.s_addr = htonl(ip);
      send_query(s, dst);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);
    std::vector<ServerListing> found;
    char buf[512];
    while (std::chrono::steady_clock::now() < deadline) {
      sockaddr_in from{};
#ifdef _WIN32
      int from_len = sizeof(from);
#else
      socklen_t from_len = sizeof(from);
#endif
      const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                             &from_len);
      if (n > 0) {
        ServerListing listing;
        if (parse_reply(buf, n, listing)) {
          char addr[INET_ADDRSTRLEN] = {};
          inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
          listing.address = addr;
          const auto it = std::find_if(found.begin(), found.end(), [&](const ServerListing& x) {
            return x.address == listing.address && x.game_port == listing.game_port;
          });
          if (it == found.end()) found.push_back(listing);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    close_socket(s);
    servers_ = std::move(found);
    scanning_ = false;
  }).detach();
}

void ServerBrowser::poll() {}

bool DiscoveryHost::start(std::uint16_t game_port) {
  wsa();
  stop();
  game_port_ = game_port;
  SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalidSocket) return false;
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(discovery_port(game_port));
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_socket(s);
    return false;
  }
  set_nonblock(s);
  socket_ = reinterpret_cast<void*>(static_cast<std::uintptr_t>(s));
  active_ = true;
  return true;
}

void DiscoveryHost::set_advert(const DiscoveryAdvert& advert) { advert_ = advert; }

void DiscoveryHost::set_broadcast_targets(bool lan, bool tailscale,
                                          const std::string& tailscale_subnet) {
  broadcast_lan_ = lan;
  broadcast_tailscale_ = tailscale;
  tailscale_subnet_ = tailscale_subnet;
}

void DiscoveryHost::send_broadcast_announce() {
  if (!active_ || !socket_) return;
  const SocketHandle s =
      static_cast<SocketHandle>(reinterpret_cast<std::uintptr_t>(socket_));
  std::string map = advert_.map_name.substr(0, 200);
  std::string host = advert_.host_name.substr(0, 48);
  std::array<char, 512> out{};
  int o = 0;
  std::memcpy(out.data() + o, kReply, 4);
  o += 4;
  out[o++] = static_cast<char>(advert_.game_port & 0xff);
  out[o++] = static_cast<char>((advert_.game_port >> 8) & 0xff);
  out[o++] = static_cast<char>(std::clamp(advert_.players, 0, 255));
  out[o++] = advert_.in_game ? 1 : 0;
  out[o++] = advert_.allow_late_join ? 1 : 0;
  const int map_len = std::min(static_cast<int>(map.size()), 200);
  out[o++] = static_cast<char>(map_len);
  std::memcpy(out.data() + o, map.data(), static_cast<std::size_t>(map_len));
  o += map_len;
  const int host_len = std::min(static_cast<int>(host.size()), 48);
  out[o++] = static_cast<char>(host_len);
  std::memcpy(out.data() + o, host.data(), static_cast<std::size_t>(host_len));
  o += host_len;

  auto send_to = [&](std::uint32_t ip) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(discovery_port(game_port_));
    dst.sin_addr.s_addr = htonl(ip);
    sendto(s, out.data(), o, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
  };
  if (broadcast_lan_) send_to(0xFFFFFFFFu);
  // Tailscale is point-to-point; browsers probe each /24 host with PDQ (we reply passively).
  // Optional: nudge our own Tailscale IP so a concurrent scan picks us up immediately.
  if (broadcast_tailscale_ && !tailscale_subnet_.empty()) {
    const std::string self = detect_local_ip();
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(self.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
      send_to((a << 24) | (b << 16) | (c << 8) | d);
    }
  }
}

void DiscoveryHost::poll() {
  if (!active_ || !socket_) return;
  broadcast_timer_ += 1.f / 60.f;
  if (broadcast_timer_ >= 2.5f) {
    broadcast_timer_ = 0.f;
    send_broadcast_announce();
  }
  const SocketHandle s =
      static_cast<SocketHandle>(reinterpret_cast<std::uintptr_t>(socket_));
  char buf[64];
  sockaddr_in from{};
#ifdef _WIN32
  int from_len = sizeof(from);
#else
  socklen_t from_len = sizeof(from);
#endif
  for (;;) {
    const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) break;
    if (n >= 4 && std::memcmp(buf, kQuery, 4) == 0) {
      std::string map = advert_.map_name.substr(0, 200);
      std::string host = advert_.host_name.substr(0, 48);
      std::array<char, 512> out{};
      int o = 0;
      std::memcpy(out.data() + o, kReply, 4);
      o += 4;
      out[o++] = static_cast<char>(advert_.game_port & 0xff);
      out[o++] = static_cast<char>((advert_.game_port >> 8) & 0xff);
      out[o++] = static_cast<char>(std::clamp(advert_.players, 0, 255));
      out[o++] = advert_.in_game ? 1 : 0;
      out[o++] = advert_.allow_late_join ? 1 : 0;
      const int map_len = std::min(static_cast<int>(map.size()), 200);
      out[o++] = static_cast<char>(map_len);
      std::memcpy(out.data() + o, map.data(), static_cast<std::size_t>(map_len));
      o += map_len;
      const int host_len = std::min(static_cast<int>(host.size()), 48);
      out[o++] = static_cast<char>(host_len);
      std::memcpy(out.data() + o, host.data(), static_cast<std::size_t>(host_len));
      o += host_len;
      sendto(s, out.data(), o, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }
  }
}

void DiscoveryHost::stop() {
  if (!socket_) return;
  close_socket(static_cast<SocketHandle>(reinterpret_cast<std::uintptr_t>(socket_)));
  socket_ = nullptr;
  active_ = false;
}

}  // namespace dalian
