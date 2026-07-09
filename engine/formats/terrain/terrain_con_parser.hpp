#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

// Parsed terrain material / tiling settings from a level's Terrain.con.
struct TerrainConData {
  float primary_world_scale_x = 2.f;
  float primary_world_scale_y = 0.00640869f;
  float primary_world_scale_z = 2.f;
  int patch_size = 128;
  int patch_colormap_size = 512;
  int low_detailmap_size = 512;
  std::string colormap_tile_prefix = "Colormaps/tx";
  std::string lightmap_tile_prefix = "Lightmaps/tx";
  std::string detailmap_tile_prefix = "Detailmaps/tx";
  std::string low_detailmap_tile_prefix;
  float far_side_tiling_x = 4.f;
  float far_side_tiling_z = 2.f;
  float far_top_tiling_hi = 10.f;
  float far_top_tiling_low = 12.f;
  // Optional per-layer detail texture overrides (index → archive vpath).
  std::unordered_map<int, std::string> detail_texture_by_layer;
};

class TerrainConParser {
public:
  void parse(const std::string& terrain_con_script);
  const TerrainConData& data() const { return data_; }

  // Resolve a BF2 base name like "Levels/Dalian_plant/Colormaps/tx" to "Colormaps/tx".
  static std::string tile_prefix_from_base_name(const std::string& quoted_base);

private:
  TerrainConData data_{};
  static void parse_vec3_token(const std::string& token, float out[3]);
};

bool terrain_con_parser_self_test();

}  // namespace bf2
