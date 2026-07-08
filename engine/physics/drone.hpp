#pragma once

// 6-Degrees-of-Freedom FPV quadcopter flight model.
//
// Unlike the legacy Refractor vehicle controller (which slides a rigid box over
// a 2D plane), this integrates a true rigid body: four rotors at the corners of
// an X-frame each produce an independent thrust along the body "up" axis. The
// player's stick inputs (throttle / pitch / roll / yaw) are mixed into the four
// rotor commands exactly like a real flight controller, so attitude emerges from
// differential thrust rather than being applied directly. Gravity, linear and
// angular drag, momentum and battery voltage sag are all modelled, giving the
// floaty, inertia-heavy handling of an actual fibre-optic FPV drone.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace bf2 {

struct DroneController {
  // ---- State ----
  glm::vec3 position{0.f, 0.f, 0.f};
  glm::vec3 velocity{0.f, 0.f, 0.f};        // world-space m/s
  glm::quat orientation{1.f, 0.f, 0.f, 0.f};
  glm::vec3 angular_velocity{0.f, 0.f, 0.f};  // body-space rad/s (pitch,yaw,roll ~ x,y,z)
  float battery = 1.0f;                        // 1 = full, 0 = empty

  // ---- Airframe parameters (roughly a 5" freestyle quad) ----
  float mass = 0.65f;              // kg
  float arm = 0.13f;              // rotor distance from centre (m)
  float max_rotor_thrust = 6.0f;  // N per rotor at full battery (~2.4 kg total)
  float inertia = 0.010f;         // kg*m^2 (uniform approx)
  float yaw_torque_coeff = 0.09f; // rotor drag torque -> yaw authority
  float linear_drag = 0.35f;      // body air resistance
  float angular_drag = 6.5f;      // rotational damping (rad/s)
  float battery_drain = 0.010f;   // charge lost per second at full throttle
  float gravity = 9.81f;

  struct Input {
    float throttle = 0.f;  // 0..1
    float pitch = 0.f;     // -1..1 (nose down/up)
    float roll = 0.f;      // -1..1 (right/left)
    float yaw = 0.f;       // -1..1 (right/left)
  };

  // Voltage sag: available thrust falls as the pack depletes (and collapses hard
  // near empty, like a real LiPo hitting its knee).
  float available_thrust() const {
    const float sag = 0.55f + 0.45f * std::clamp(battery * 1.05f, 0.f, 1.f);
    return max_rotor_thrust * sag;
  }

  // Advance one physics step. `ground_height` is the terrain height under the
  // drone so it can rest / bounce on the surface.
  void update(const Input& in, float dt, float ground_height) {
    if (dt <= 0.f) return;
    const float t_cmd = std::clamp(in.throttle, 0.f, 1.f);
    const float p_cmd = std::clamp(in.pitch, -1.f, 1.f);
    const float r_cmd = std::clamp(in.roll, -1.f, 1.f);
    const float y_cmd = std::clamp(in.yaw, -1.f, 1.f);

    // ---- Motor mix (X-frame). Rotors: 0=FL 1=FR 2=RL 3=RR ----
    const float base = t_cmd;
    const float pitchA = p_cmd * 0.5f;  // + = pitch forward (front rotors down)
    const float rollA = r_cmd * 0.5f;   // + = roll right
    const float yawA = y_cmd * 0.5f;
    float m[4];
    m[0] = base - pitchA + rollA + yawA;  // front-left
    m[1] = base - pitchA - rollA - yawA;  // front-right
    m[2] = base + pitchA + rollA - yawA;  // rear-left
    m[3] = base + pitchA - rollA + yawA;  // rear-right
    const float tmax = available_thrust();
    for (float& v : m) v = std::clamp(v, 0.f, 1.f) * tmax;

    // ---- Linear dynamics ----
    const float total_thrust = m[0] + m[1] + m[2] + m[3];
    const glm::vec3 body_up = orientation * glm::vec3(0.f, 1.f, 0.f);
    glm::vec3 force = body_up * total_thrust;
    force.y -= gravity * mass;
    force -= velocity * linear_drag;  // air resistance
    velocity += (force / mass) * dt;
    position += velocity * dt;

    // ---- Angular dynamics: torque from differential thrust ----
    // Pitch torque: (rear - front); Roll torque: (left - right); Yaw: prop drag.
    const float pitch_torque = ((m[2] + m[3]) - (m[0] + m[1])) * arm;
    const float roll_torque = ((m[0] + m[2]) - (m[1] + m[3])) * arm;
    const float yaw_torque = ((m[0] + m[3]) - (m[1] + m[2])) * yaw_torque_coeff;
    glm::vec3 torque(pitch_torque, yaw_torque, roll_torque);
    glm::vec3 ang_accel = torque / inertia;
    angular_velocity += ang_accel * dt;
    angular_velocity -= angular_velocity * std::min(angular_drag * dt, 1.0f);  // damping

    // Integrate orientation by the body-space angular velocity.
    const glm::vec3 w = angular_velocity * dt;
    const glm::quat dq(1.f, w.x * 0.5f, w.y * 0.5f, w.z * 0.5f);
    orientation = glm::normalize(orientation * dq);

    // ---- Battery drain (throttle-proportional) ----
    battery = std::max(0.f, battery - battery_drain * (0.3f + 0.7f * t_cmd) * dt);

    // ---- Ground contact ----
    const float floor = ground_height + 0.10f;
    if (position.y < floor) {
      position.y = floor;
      if (velocity.y < 0.f) velocity.y *= -0.25f;  // soft bounce
      velocity.x *= 0.80f;
      velocity.z *= 0.80f;
      // Level out gradually when resting on the ground.
      angular_velocity *= 0.6f;
    }
  }

  // Forward/up basis for an FPV camera. Real FPV cams are tilted up so the drone
  // can fly fast while the pilot still sees ahead; `cam_tilt_deg` applies that.
  glm::vec3 forward(float cam_tilt_deg = 25.f) const {
    const float a = glm::radians(cam_tilt_deg);
    const glm::vec3 local(0.f, std::sin(a), -std::cos(a));
    return orientation * local;
  }
  glm::vec3 up() const { return orientation * glm::vec3(0.f, 1.f, 0.f); }
};

}  // namespace bf2
