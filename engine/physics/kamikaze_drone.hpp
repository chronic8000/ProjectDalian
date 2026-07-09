#pragma once

// FPV loitering munition — a one-way kamikaze quadcopter.
//
// Reuses the 6-DoF DroneController flight model but with a lighter, more
// aggressive airframe tuned for terminal dives into hatches and windows. The
// operator steers via the same FPV link; impact or manual detonation triggers
// a shaped charge blast. There is no recall — once launched, it flies until it
// hits something or the battery dies.

#include "drone.hpp"

namespace bf2 {

struct KamikazeDrone {
  DroneController body{};

  // Apply loitering-munition tuning on top of a fresh DroneController.
  static KamikazeDrone spawn() {
    KamikazeDrone k;
    auto& d = k.body;
    d.mass = 0.32f;              // lighter than a freestyle quad
    d.arm = 0.085f;
    d.max_rotor_thrust = 10.5f;  // punchy for terminal manoeuvres
    d.inertia = 0.005f;
    d.yaw_torque_coeff = 0.16f;  // fast yaw for window strikes
    d.linear_drag = 0.26f;
    d.angular_drag = 9.0f;
    d.battery_drain = 0.032f;    // ~30 s of powered flight
    d.battery = 1.0f;
    d.position = {0.f, 0.f, 0.f};
    d.velocity = {0.f, 0.f, 0.f};
    d.orientation = {1.f, 0.f, 0.f, 0.f};
    d.angular_velocity = {0.f, 0.f, 0.f};
    return k;
  }

  void update(const DroneController::Input& in, float dt, float ground_height) {
    body.update(in, dt, ground_height);
  }

  glm::vec3 forward(float cam_tilt_deg = 20.f) const { return body.forward(cam_tilt_deg); }
  glm::vec3 up() const { return body.up(); }
  glm::vec3 position() const { return body.position; }
  float battery() const { return body.battery; }
};

}  // namespace bf2
