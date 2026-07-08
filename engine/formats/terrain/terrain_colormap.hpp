#pragma once

#include <cstdint>
#include <string>

#include "engine/core/resource_manager.hpp"
#include "engine/formats/terrain/terrain_loader.hpp"

namespace bf2 {

struct TerrainVisualConfig {
  std::string colormap_folder = "colormaps";
  std::string lightmap_folder = "lightmaps";
  std::string detail_folder = "detailmaps";
  int tile_cols = 0;
  int tile_rows = 0;
  int tile_size = 512;
};

// Stitched CPU-side RGBA atlases (safe to build before GL init).
struct TerrainGroundAtlases {
  DdsTexture colormap;
  DdsTexture lightmap;
  // Per-patch blend masks (green channel over white base) stitched like the
  // colormap; drive the near-detail splat.
  DdsTexture mask1;
  DdsTexture mask2;
  // Shared tiling detail ground textures (base / layer1 / layer2).
  DdsTexture detail0;
  DdsTexture detail1;
  DdsTexture detail2;
  bool has_lightmap = false;
  bool has_mask1 = false;
  bool has_mask2 = false;
  bool has_detail0 = false;
  bool has_detail1 = false;
  bool has_detail2 = false;
  int tile_cols = 0;
  int tile_rows = 0;
};

// GPU texture handles uploaded from atlases (requires active GL context).
struct TerrainGroundTextures {
  std::uint32_t colormap = 0;
  std::uint32_t lightmap = 0;
  std::uint32_t mask1 = 0;    // blend weight atlas (GL_CLAMP)
  std::uint32_t mask2 = 0;    // blend weight atlas (GL_CLAMP)
  std::uint32_t detail0 = 0;  // base tiling detail (GL_REPEAT)
  std::uint32_t detail1 = 0;  // layer1 tiling detail (GL_REPEAT)
  std::uint32_t detail2 = 0;  // layer2 tiling detail (GL_REPEAT)
  bool splat = false;         // true when per-patch detail splat is available
  int tile_cols = 0;
  int tile_rows = 0;
};

class TerrainColormapLoader {
public:
  static TerrainVisualConfig parse_terrain_con(const std::string& script);
  // Stitch tile DDS files into atlases on the CPU (no GL required).
  static TerrainGroundAtlases build_atlases(ResourceManager& resources, const TerrainVisualConfig& cfg);
  // Upload atlases to GL (call after a context is current).
  static TerrainGroundTextures upload(const TerrainGroundAtlases& atlases);
  static TerrainGroundTextures load(ResourceManager& resources, const TerrainVisualConfig& cfg);
  static void destroy(TerrainGroundTextures& textures);
};

TexturedMeshData terrain_to_textured_mesh(const Terrain& terrain, float xz_scale, int step);

// Parse heightmap.setScale x/y/z from Heightdata.con (returns xz horizontal spacing).
float parse_heightmap_xz_scale(const std::string& heightdata_script, float default_xz = 2.0f);

}  // namespace bf2
