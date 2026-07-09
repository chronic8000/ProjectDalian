#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace bf2 {

// One placed static mesh from StaticObjects.con (buildings, bridges, props).
struct StaticEntity {
  std::string template_name;
  glm::vec3 position{0.f};
  glm::vec3 rotation_euler_deg{0.f};  // yaw / pitch / roll (BF2 order)
  glm::quat rotation{1.f, 0.f, 0.f, 0.f};
  int layer = 0;
};

// Parses StaticObjects.con placement blocks. Does not follow `run` lines — pair with
// TemplateResolver to map template_name → mesh archive path.
class StaticObjectParser {
public:
  void clear();
  void parse(const std::string& static_objects_script);
  const std::vector<StaticEntity>& entities() const { return entities_; }
  std::vector<std::string> run_paths() const;

  // Match main.cpp / level placement_matrix: yaw(Y) → pitch(X) → roll(Z).
  static glm::quat rotation_from_euler_deg(const glm::vec3& yaw_pitch_roll);

private:
  std::vector<StaticEntity> entities_;
  std::vector<std::string> run_paths_;
};

bool static_object_parser_self_test();

}  // namespace bf2
