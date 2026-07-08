#include "level_loader.hpp"

#include <filesystem>
#include <sstream>
#include <string>

namespace bf2 {
namespace {

// Extract the first slash-separated float triplet's Y and the size/scale from a
// Heightdata.con-style script. Returns width/height/vertical-scale via out params.
bool parse_heightdata(const std::string& script, std::uint32_t& w, std::uint32_t& h,
                      float& vscale, float& xz_scale) {
  std::istringstream in(script);
  std::string line;
  bool found = false;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;
    if (cmd == "heightmap.setSize") {
      ls >> w >> h;
      found = true;
    } else if (cmd == "heightmap.setScale") {
      std::string triple;
      ls >> triple;
      // "x/y/z" -> xz_scale = x, vscale = y
      const auto s1 = triple.find('/');
      const auto s2 = triple.find('/', s1 + 1);
      if (s1 != std::string::npos && s2 != std::string::npos) {
        try {
          xz_scale = std::stof(triple.substr(0, s1));
          vscale = std::stof(triple.substr(s1 + 1, s2 - s1 - 1));
        } catch (...) {
        }
      }
      break;  // only the primary heightmap (first block) matters here
    }
  }
  return found;
}

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

  // Placements.
  if (const auto so = resources_.read_bytes("StaticObjects.con")) {
    std::string script(reinterpret_cast<const char*>(so->data()), so->size());
    interpreter_.execute_script(script);
  }
  result.placements = interpreter_.instances();

  // Terrain (primary heightmap).
  std::uint32_t w = 1025;
  std::uint32_t h = 1025;
  float vscale = 0.00640869f;
  float xz_scale = 2.0f;
  if (const auto hd = resources_.read_bytes("Heightdata.con")) {
    std::string script(reinterpret_cast<const char*>(hd->data()), hd->size());
    parse_heightdata(script, w, h, vscale, xz_scale);
  }
  if (const auto raw = resources_.read_bytes("HeightmapPrimary.raw")) {
    try {
      result.terrain = TerrainLoader::load_raw_heightmap(*raw, w, h, vscale);
      result.has_terrain = true;
    } catch (...) {
      result.has_terrain = false;
    }
  }

  return result;
}

}  // namespace bf2
