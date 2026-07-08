#include "network_stub.hpp"

namespace bf2 {

bool NetworkStub::start_server(std::uint16_t port) {
  server_mode_ = true;
  (void)port;
  return true;
}

bool NetworkStub::connect_client(const std::string& host, std::uint16_t port) {
  server_mode_ = false;
  (void)host;
  (void)port;
  return true;
}

void NetworkStub::publish_state(const NetEntityState& state) {
  for (auto& existing : replicated_states_) {
    if (existing.id == state.id) {
      existing = state;
      return;
    }
  }
  replicated_states_.push_back(state);
}

std::vector<NetEntityState> NetworkStub::poll_states() { return replicated_states_; }

void NetworkStub::shutdown() {
  replicated_states_.clear();
  server_mode_ = false;
}

}  // namespace bf2
