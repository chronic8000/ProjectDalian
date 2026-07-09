#pragma once

#include "terrain_loader.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bf2 {

// BF2 levels use a 3x3 heightmap cluster: one high-res primary patch (1025² @ 2 m)
// surrounded by eight lower-res secondary patches (257² @ 8 m). Each patch covers
// 2048 m of world space; the primary is centred on the origin.
struct HeightmapPatch {
  int grid_x = 0;
  int grid_y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  float xz_scale = 2.f;
  float vscale = 1.f;
  int bit_resolution = 16;
  Terrain terrain;
};

class HeightmapCluster {
public:
  bool load_from_script(class ResourceManager& resources, const std::string& heightdata_con);

  using RawReadFn =
      std::function<std::optional<std::vector<std::uint8_t>>(const std::string& basename)>;
  bool load_from_script_with_reader(const std::string& heightdata_con, const RawReadFn& read_fn);

  bool valid() const { return !patches_.empty(); }
  const std::vector<HeightmapPatch>& patches() const { return patches_; }

  // World-space bilinear height (metres). Falls back to 0 outside all patches.
  float sample_height(float world_x, float world_z) const;

  // Merge the cluster into a single Terrain grid for mesh rendering / legacy paths.
  // step = world metres between merged samples (4 is a good default).
  Terrain build_merged_terrain(float step = 4.f) const;

private:
  std::vector<HeightmapPatch> patches_;
  const HeightmapPatch* patch_at(int gx, int gy) const;
  void patch_world_bounds(const HeightmapPatch& p, float& min_x, float& min_z, float& max_x,
                          float& max_z) const;
};

bool heightmap_cluster_self_test();

}  // namespace bf2
