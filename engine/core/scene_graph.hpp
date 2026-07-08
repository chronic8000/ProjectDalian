#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

struct SceneObject {
  std::string name;
  std::string mesh_path;
  Float3 position{};
  Float3 rotation{};
  Float3 scale{1.f, 1.f, 1.f};
  ExtractedMesh geometry;
};

class SceneGraph {
public:
  SceneObject& add_object(std::string name, std::string mesh_path, ExtractedMesh geometry);
  const std::vector<SceneObject>& objects() const { return objects_; }
  void clear() { objects_.clear(); }

private:
  std::vector<SceneObject> objects_;
};

}  // namespace bf2
