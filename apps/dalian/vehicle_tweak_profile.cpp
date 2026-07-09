#include "vehicle_tweak_profile.hpp"

#include "vehicle_air_profile.hpp"

namespace dalian {

VehicleTweakProfile parse_vehicle_tweak(const std::string& tweak_text) {
  VehicleTweakProfile p;
  const VehicleAirProfile air = parse_vehicle_air_profile(tweak_text);
  if (!air.valid) return p;
  p.gear_up_speed = air.gear_up_speed;
  p.gear_down_speed = air.gear_down_speed;
  p.gear_up_height = air.gear_up_height;
  p.max_thrust = air.max_thrust;
  p.wing_lift = air.physics_wing_lift > 0.f ? air.physics_wing_lift : air.total_wing_lift;
  p.sprint_factor = air.sprint_factor;
  p.valid = true;
  return p;
}

}  // namespace dalian
