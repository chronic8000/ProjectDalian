#pragma once

#include <string>

namespace dalian {

struct VehicleWingProfile {
  float total_wing_lift = 0.62f;
  float max_flap_lift = 11.f;
  int wing_count = 0;
};

// Sum setWingLift / setFlapLift from all Wing templates in a vehicle .tweak.
VehicleWingProfile parse_vehicle_wings(const std::string& tweak_text);

}  // namespace dalian
