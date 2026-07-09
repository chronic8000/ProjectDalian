#pragma once

#include <cctype>
#include <string>

namespace dalian {

inline std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline bool vehicle_is_helicopter(const std::string& mesh_path,
                                  const std::string& spawn_name = {}) {
  const std::string path = lower_copy(mesh_path);
  const std::string spawn = lower_copy(spawn_name);
  auto has = [&](const char* needle) {
    return path.find(needle) != std::string::npos || spawn.find(needle) != std::string::npos;
  };
  if (has("helo") || has("helicopter") || has("rotorwing") || has("rotary")) return true;
  if (has("uh60") || has("uh1") || has("uh_1") || has("blackhawk")) return true;
  if (has("z8") || has("mi17") || has("mi-17") || has("mi_17")) return true;
  if (has("apache") || has("hind") || has("littlebird") || has("lilbird")) return true;
  if (has("the_uh") || has("the_z8") || has("the_z") || has("the_mi")) return true;
  if (spawn.find("usthe_") != std::string::npos || spawn.find("chthe_") != std::string::npos)
    return true;
  return false;
}

inline bool vehicle_is_aircraft(const std::string& mesh_path, const std::string& spawn_name = {}) {
  if (vehicle_is_helicopter(mesh_path, spawn_name)) return true;
  const std::string path = lower_copy(mesh_path);
  const std::string spawn = lower_copy(spawn_name);
  if (path.find("loaded_vehicles/air/") != std::string::npos) return true;
  auto has = [&](const char* needle) {
    return path.find(needle) != std::string::npos || spawn.find(needle) != std::string::npos;
  };
  if (has("f16") || has("f18") || has("f35") || has("f/a") || has("mig") || has("su30") ||
      has("su-30") || has("j10") || has("jet") || has("faa") || has("harrier") ||
      has("f15") || has("f22") || has("eurofighter"))
    return true;
  return false;
}

inline bool vehicle_is_boat(const std::string& mesh_path, const std::string& spawn_name = {}) {
  const std::string path = lower_copy(mesh_path);
  const std::string spawn = lower_copy(spawn_name);
  if (path.find("loaded_vehicles/sea/") != std::string::npos) return true;
  auto has = [&](const char* needle) {
    return path.find(needle) != std::string::npos || spawn.find(needle) != std::string::npos;
  };
  if (has("boat") || has("rib") || has("rhib") || has("zodiac") || has("patrolboat")) return true;
  if (spawn.find("boat_") != std::string::npos) return true;
  return false;
}

}  // namespace dalian
