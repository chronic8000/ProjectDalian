#pragma once

#include "game_sim_types.hpp"

#include "engine/script/con_interpreter.hpp"

#include <string>
#include <vector>

namespace dalian {

struct AmbientEmitter {
  glm::vec3 pos{};
  glm::vec3 dir{0.f, 1.f, 0.f};
  std::string bundle;
  std::uint8_t kind = 0;  // 0 smoke, 1 water spray, 2 glow light
  float emit_period = 1.f / 30.f;
  float accum = 0.f;
  glm::vec3 light_color{1.f, 0.92f, 0.7f};
  float light_radius = 14.f;
};

struct SceneLight {
  glm::vec3 pos{};
  glm::vec3 color{1.f, 0.92f, 0.7f};
  float radius = 14.f;
};

bool is_ambient_emitter_template(const std::string& template_name);
std::string ambient_emitter_bundle(const std::string& template_name);
std::vector<AmbientEmitter> collect_ambient_emitters(
    const std::vector<bf2::ObjectInstance>& placements);
std::vector<SceneLight> collect_scene_lights(const std::vector<AmbientEmitter>& emitters);
void step_ambient_emitters(std::vector<AmbientEmitter>& emitters, std::vector<Smoke>& smoke,
                           float dt);
bool ambient_emitter_self_test();

}  // namespace dalian
