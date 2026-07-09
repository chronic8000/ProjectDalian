#include "vehicle_wing_profile.hpp"

#include <algorithm>
#include <sstream>

namespace dalian {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

float parse_float(const std::string& s) {
  try {
    return std::stof(s);
  } catch (...) {
    return 0.f;
  }
}

}  // namespace

VehicleWingProfile parse_vehicle_wings(const std::string& tweak_text) {
  VehicleWingProfile p;
  std::istringstream in(tweak_text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd, val;
    if (!(ls >> cmd)) continue;
    const std::string c = lower(cmd);
    if (c == "objecttemplate.create" || c == "objecttemplate.activesafe") {
      std::string type, name;
      ls >> type >> name;
      if (lower(type) == "wing") ++p.wing_count;
    } else if (c == "objecttemplate.setwinglift") {
      if (ls >> val) p.total_wing_lift = std::max(p.total_wing_lift, parse_float(val));
    } else if (c == "objecttemplate.setflaplift") {
      if (ls >> val) p.max_flap_lift = std::max(p.max_flap_lift, parse_float(val));
    }
  }
  return p;
}

}  // namespace dalian
