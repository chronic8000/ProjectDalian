#include "terrain_con_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace bf2 {
namespace {

std::string strip_quotes(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
  return s;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

void TerrainConParser::parse_vec3_token(const std::string& token, float out[3]) {
  std::size_t start = 0;
  for (int i = 0; i < 3; ++i) {
    const auto slash = token.find('/', start);
    const std::string part = token.substr(start, slash - start);
    try {
      out[i] = part.empty() ? 0.f : std::stof(part);
    } catch (...) {
      out[i] = 0.f;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
}

std::string TerrainConParser::tile_prefix_from_base_name(const std::string& quoted_base) {
  std::string path = lower(strip_quotes(quoted_base));
  for (auto& c : path) {
    if (c == '\\') c = '/';
  }
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return path;
  const auto prev = path.rfind('/', slash - 1);
  if (prev == std::string::npos) return path.substr(0, slash + 1) + path.substr(slash + 1);
  const std::string folder = path.substr(prev + 1, slash - prev - 1);
  const std::string tile = path.substr(slash + 1);
  // Preserve canonical BF2 folder casing for display; archive lookup is case-insensitive.
  char folder_c[32];
  if (!folder.empty()) {
    folder_c[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(folder[0])));
    for (std::size_t i = 1; i < folder.size() && i < sizeof(folder_c) - 1; ++i) {
      folder_c[i] = folder[i];
    }
    folder_c[std::min(folder.size(), sizeof(folder_c) - 1)] = '\0';
    return std::string(folder_c) + "/" + tile;
  }
  return tile;
}

void TerrainConParser::parse(const std::string& terrain_con_script) {
  data_ = TerrainConData{};
  std::istringstream in(terrain_con_script);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;
    if (cmd == "terrain.primaryWorldScale") {
      std::string triple;
      ls >> triple;
      float s[3]{};
      parse_vec3_token(triple, s);
      data_.primary_world_scale_x = s[0];
      data_.primary_world_scale_y = s[1];
      data_.primary_world_scale_z = s[2];
    } else if (cmd == "terrain.patchSize") {
      ls >> data_.patch_size;
    } else if (cmd == "terrain.patchColormapSize") {
      ls >> data_.patch_colormap_size;
    } else if (cmd == "terrain.lowDetailmapSize") {
      ls >> data_.low_detailmap_size;
    } else if (cmd == "terrain.colormapBaseName") {
      std::string base;
      ls >> base;
      data_.colormap_tile_prefix = tile_prefix_from_base_name(base);
    } else if (cmd == "terrain.lightmapBaseName") {
      std::string base;
      ls >> base;
      data_.lightmap_tile_prefix = tile_prefix_from_base_name(base);
    } else if (cmd == "terrain.detailmapBaseName") {
      std::string base;
      ls >> base;
      data_.detailmap_tile_prefix = tile_prefix_from_base_name(base);
    } else if (cmd == "terrain.lowDetailmapBaseName") {
      std::string base;
      ls >> base;
      data_.low_detailmap_tile_prefix = tile_prefix_from_base_name(base);
    } else if (cmd == "terrain.farSideTiling") {
      std::string triple;
      ls >> triple;
      float t[3]{};
      parse_vec3_token(triple, t);
      data_.far_side_tiling_x = t[0];
      data_.far_side_tiling_z = t[2];
    } else if (cmd == "terrain.farTopTilingHi") {
      ls >> data_.far_top_tiling_hi;
    } else if (cmd == "terrain.farTopTilingLow") {
      ls >> data_.far_top_tiling_low;
    } else if (cmd == "terrain.mapDetailTexture" || cmd == "terrain.setDetailTexture") {
      int layer = 0;
      std::string path;
      ls >> layer >> path;
      data_.detail_texture_by_layer[layer] = strip_quotes(path);
    }
  }
}

bool terrain_con_parser_self_test() {
  static const char* kSample = R"(
terrain.create Terrain
terrain.primaryWorldScale 2/0.00640869/2
terrain.patchColormapSize 512
terrain.detailmapBaseName "Levels/Dalian_plant/Detailmaps/tx"
terrain.colormapBaseName "Levels/Dalian_plant/Colormaps/tx"
terrain.lightmapBaseName "Levels/Dalian_plant/Lightmaps/tx"
)";

  TerrainConParser parser;
  parser.parse(kSample);
  const auto& d = parser.data();
  if (d.colormap_tile_prefix != "Colormaps/tx") return false;
  if (d.detailmap_tile_prefix != "Detailmaps/tx") return false;
  if (std::fabs(d.primary_world_scale_x - 2.f) > 1e-5f) return false;
  if (d.patch_colormap_size != 512) return false;
  return true;
}

}  // namespace bf2
