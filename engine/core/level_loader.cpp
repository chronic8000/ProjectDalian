#include "level_loader.hpp"

#include "engine/core/static_object_parser.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace bf2 {
namespace {

}  // namespace

LevelLoader::LevelLoader(ResourceManager& resources) : resources_(resources) {}

LevelLoadResult LevelLoader::load_static_objects(const std::string& static_objects_con) {
  LevelLoadResult result;
  result.level_name = static_objects_con;
  interpreter_.execute_file(static_objects_con);
  result.placements = interpreter_.instances();
  return result;
}

LevelLoadResult LevelLoader::load_level_directory(const std::string& level_dir) {
  LevelLoadResult result;
  result.level_name = level_dir;
  const auto static_objects = (std::filesystem::path(level_dir) / "StaticObjects.con").string();
  if (std::filesystem::exists(static_objects)) {
    interpreter_.execute_file(static_objects);
  }
  result.placements = interpreter_.instances();
  return result;
}

LevelLoadResult LevelLoader::load_mounted_level(const std::string& level_name) {
  LevelLoadResult result;
  result.level_name = level_name;

  // Placements from StaticObjects.con (+ DummyObjects.con when present).
  if (const auto so = resources_.read_bytes("StaticObjects.con")) {
    std::string script(reinterpret_cast<const char*>(so->data()), so->size());
    interpreter_.execute_script(script);
    StaticObjectParser parser;
    parser.parse(script);
    if (parser.entities().size() != interpreter_.instances().size()) {
      std::cerr << "StaticObjectParser: " << parser.entities().size()
                << " entities vs ConInterpreter " << interpreter_.instances().size()
                << " instances\n";
    }
  }
  if (const auto dummy = resources_.read_bytes("DummyObjects.con")) {
    const std::size_t before = interpreter_.instances().size();
    std::string script(reinterpret_cast<const char*>(dummy->data()), dummy->size());
    interpreter_.execute_script(script);
    std::cout << "DummyObjects: merged " << (interpreter_.instances().size() - before)
              << " extra placements\n";
  }
  result.placements = interpreter_.instances();

  // Compiled road meshes (client.zip).
  if (const auto roads = resources_.read_bytes("CompiledRoads.con")) {
    const std::string script(reinterpret_cast<const char*>(roads->data()), roads->size());
    result.roads = parse_compiled_roads(script);
    std::cout << "CompiledRoads: " << result.roads.size() << " segments\n";
  }

  // Full 3x3 heightmap cluster (primary + eight secondaries).
  if (const auto hd = resources_.read_bytes("Heightdata.con")) {
    const std::string script(reinterpret_cast<const char*>(hd->data()), hd->size());
    if (result.heightmap_cluster.load_from_script(resources_, script)) {
      result.has_heightmap_cluster = true;
      std::cout << "HeightmapCluster: " << result.heightmap_cluster.patches().size()
                << " patches loaded\n";
    }
  }

  // Primary heightfield for terrain mesh / colormap UVs (centred grid).
  if (result.has_heightmap_cluster) {
    for (const auto& patch : result.heightmap_cluster.patches()) {
      if (patch.grid_x == 0 && patch.grid_y == 0) {
        result.terrain = patch.terrain;
        result.has_terrain = !result.terrain.samples.empty();
        break;
      }
    }
  }
  if (!result.has_terrain) {
    std::uint32_t w = 1025;
    std::uint32_t h = 1025;
    float vscale = 0.00640869f;
    if (const auto raw = resources_.read_bytes("HeightmapPrimary.raw")) {
      try {
        result.terrain = TerrainLoader::load_raw_heightmap(*raw, w, h, vscale);
        result.has_terrain = true;
      } catch (...) {
        result.has_terrain = false;
      }
    }
  }

  return result;
}

}  // namespace bf2
