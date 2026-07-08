#include "scene_graph.hpp"

namespace bf2 {

SceneObject& SceneGraph::add_object(std::string name, std::string mesh_path, ExtractedMesh geometry) {
  SceneObject object;
  object.name = std::move(name);
  object.mesh_path = std::move(mesh_path);
  object.geometry = std::move(geometry);
  objects_.push_back(std::move(object));
  return objects_.back();
}

}  // namespace bf2
