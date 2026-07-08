#pragma once

#include <string>
#include <unordered_map>

namespace bf2 {

class GameplayScript {
public:
  bool initialize();
  void shutdown();
  void bind_float(const std::string& name, float value);
  void call(const std::string& function);
  float get_float(const std::string& name) const;

private:
  std::unordered_map<std::string, float> state_;
};

}  // namespace bf2
