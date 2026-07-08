#pragma once

#include <string>
#include <vector>

#include "engine/core/resource_manager.hpp"
#include "engine/core/scene_graph.hpp"
#include "engine/formats/terrain/terrain_loader.hpp"
#include "engine/script/con_interpreter.hpp"

namespace bf2 {

struct LevelLoadResult {
  SceneGraph scene;
  std::string level_name;
  std::vector<ObjectInstance> placements;
  bool has_terrain = false;
  Terrain terrain;
};

class LevelLoader {
public:
  explicit LevelLoader(ResourceManager& resources);

  LevelLoadResult load_static_objects(const std::string& static_objects_con);
  LevelLoadResult load_level_directory(const std::string& level_dir);

  // Load a level whose data has already been mounted into the ResourceManager's
  // archives (e.g. a level's server.zip). Reads StaticObjects.con placements and,
  // when present, the primary heightmap described by Heightdata.con.
  LevelLoadResult load_mounted_level(const std::string& level_name);

private:
  ResourceManager& resources_;
  ConInterpreter interpreter_;
};

}  // namespace bf2
