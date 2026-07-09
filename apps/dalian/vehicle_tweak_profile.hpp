#pragma once

#include "tweak_parser.hpp"

#include <string>

namespace dalian {

struct VehicleTweakProfile {
  float gear_up_speed = 45.f;     // deg/s — from SetGearUpSpeed
  float gear_down_speed = 35.f;
  float gear_up_height = 10.f;
  float max_thrust = 120.f;
  float wing_lift = 1.f;
  float sprint_factor = 1.6f;
  bool valid = false;
};

VehicleTweakProfile parse_vehicle_tweak(const std::string& tweak_text);

}  // namespace dalian
