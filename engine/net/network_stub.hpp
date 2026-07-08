#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf2 {

struct NetEntityState {
  std::uint32_t id = 0;
  float position[3]{};
  float rotation[3]{};
};

class NetworkStub {
public:
  bool start_server(std::uint16_t port);
  bool connect_client(const std::string& host, std::uint16_t port);
  void publish_state(const NetEntityState& state);
  std::vector<NetEntityState> poll_states();
  void shutdown();

private:
  bool server_mode_ = false;
  std::vector<NetEntityState> replicated_states_;
};

}  // namespace bf2
