#pragma once

// Guided / ballistic missile flight model.
//
// This reuses the same rigid-body integration philosophy as the FPV drone
// (`drone.hpp`): rather than teleporting a projectile along a fixed line, we
// advance position and velocity under thrust, gravity and quadratic air drag.
// In *guided* mode the flight direction is steered toward a target point with a
// limited turn rate, producing a realistic curved intercept (proportional
// pursuit). In *ballistic* mode there is no steering, so gravity alone shapes a
// lofted arc. A short rocket-motor boost phase accelerates the round, after
// which it coasts and slowly bleeds speed to drag.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace bf2 {

struct MissileController {
  // ---- State ----
  glm::vec3 position{0.f, 0.f, 0.f};
  glm::vec3 velocity{0.f, 0.f, 0.f};  // world m/s
  glm::vec3 target{0.f, 0.f, 0.f};    // homing aim point (updated by the caller)
  bool guided = true;
  bool has_target = false;
  float age = 0.f;
  float life = 9.0f;  // self-destruct time (s)
  bool alive = true;

  // ---- Airframe (roughly a 9M311 / SA-19: heavy, high-thrust boost) ----
  float mass = 42.f;             // kg
  float boost_accel = 620.f;     // m/s^2 during the motor burn
  float boost_time = 1.7f;       // powered-flight duration (s)
  float max_speed = 900.f;       // terminal velocity (m/s)
  float turn_rate = 2.8f;        // max steering rate when guided (rad/s)
  float drag = 0.00055f;         // quadratic drag coefficient
  float gravity = 9.81f;

  // Unit flight direction (falls back to +Z before the round gets moving).
  glm::vec3 forward() const {
    const float s = glm::length(velocity);
    return s > 1e-4f ? velocity / s : glm::vec3(0.f, 0.f, 1.f);
  }

  void update(float dt) {
    if (!alive || dt <= 0.f) return;
    age += dt;
    if (age >= life) {
      alive = false;
      return;
    }

    float speed = glm::length(velocity);
    glm::vec3 dir = speed > 1e-4f ? velocity / speed : glm::vec3(0.f, 0.f, 1.f);

    // ---- Guidance: rotate the flight direction toward the target, capped by
    // the airframe's turn rate so it can't snap instantly (curved intercept).
    if (guided && has_target) {
      const glm::vec3 to = target - position;
      const float d = glm::length(to);
      if (d > 1e-3f) {
        const glm::vec3 desired = to / d;
        const float cosang = glm::clamp(glm::dot(dir, desired), -1.f, 1.f);
        const float ang = std::acos(cosang);
        if (ang > 1e-4f) {
          const float step = std::min(ang, turn_rate * dt);
          glm::vec3 axis = glm::cross(dir, desired);
          const float axlen = glm::length(axis);
          if (axlen > 1e-5f) {
            axis /= axlen;
            dir = glm::normalize(glm::angleAxis(step, axis) * dir);
          }
        }
      }
    }

    // ---- Speed dynamics: motor boost, then drag-limited coast ----
    const float thrust_accel = (age < boost_time) ? boost_accel : 0.f;
    speed += thrust_accel * dt;
    speed -= drag * speed * speed * dt;
    speed = std::clamp(speed, 0.f, max_speed);

    // ---- Compose velocity, apply gravity, integrate position ----
    velocity = dir * speed;
    velocity.y -= gravity * dt;  // gravity acts on the body (arc for unguided)
    position += velocity * dt;
  }
};

}  // namespace bf2
