#include "heightmap_cluster.hpp"

#include "engine/core/resource_manager.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string basename_from_path(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos) path = path.substr(slash + 1);
  return path;
}

bool parse_scale_triple(const std::string& triple, float& xz, float& vy) {
  const auto s1 = triple.find('/');
  const auto s2 = triple.find('/', s1 + 1);
  if (s1 == std::string::npos || s2 == std::string::npos) return false;
  try {
    xz = std::stof(triple.substr(0, s1));
    vy = std::stof(triple.substr(s1 + 1, s2 - s1 - 1));
    return true;
  } catch (...) {
    return false;
  }
}

Terrain load_patch_raw(const std::vector<std::uint8_t>& data, std::uint32_t w, std::uint32_t h,
                       float vscale, int bits) {
  Terrain terrain;
  terrain.width = w;
  terrain.height = h;
  terrain.scale = vscale;
  terrain.samples.resize(static_cast<std::size_t>(w) * h);

  if (bits <= 8) {
    const std::size_t need = static_cast<std::size_t>(w) * h;
    if (data.size() < need) return {};
    for (std::size_t i = 0; i < need; ++i) {
      terrain.samples[i].height = static_cast<float>(data[i]) * vscale;
    }
    return terrain;
  }

  return TerrainLoader::load_raw_heightmap(data, w, h, vscale);
}

}  // namespace

bool HeightmapCluster::load_from_script(ResourceManager& resources,
                                        const std::string& heightdata_con) {
  return load_from_script_with_reader(
      heightdata_con, [&](const std::string& name) { return resources.read_bytes(name); });
}

bool HeightmapCluster::load_from_script_with_reader(const std::string& heightdata_con,
                                                    const RawReadFn& read_fn) {
  patches_.clear();

  struct PendingPatch {
    int gx = 0;
    int gy = 0;
    std::uint32_t w = 1025;
    std::uint32_t h = 1025;
    float xz = 2.f;
    float vy = 0.00640869f;
    int bits = 16;
    std::string raw_file;
  };
  PendingPatch cur;
  bool in_patch = false;

  std::istringstream in(heightdata_con);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string lc = lower(cmd);

    if (lc == "heightmapcluster.addheightmap") {
      std::string type;
      ls >> type >> cur.gx >> cur.gy;
      in_patch = true;
      cur.w = 1025;
      cur.h = 1025;
      cur.xz = 2.f;
      cur.vy = 0.00640869f;
      cur.bits = 16;
      cur.raw_file.clear();
    } else if (in_patch && lc == "heightmap.setsize") {
      ls >> cur.w >> cur.h;
    } else if (in_patch && lc == "heightmap.setscale") {
      std::string triple;
      ls >> triple;
      parse_scale_triple(triple, cur.xz, cur.vy);
    } else if (in_patch && lc == "heightmap.setbitresolution") {
      ls >> cur.bits;
    } else if (in_patch && lc == "heightmap.loadheightdata") {
      std::string path;
      ls >> path;
      cur.raw_file = basename_from_path(path);

      if (const auto bytes = read_fn(cur.raw_file)) {
        HeightmapPatch patch;
        patch.grid_x = cur.gx;
        patch.grid_y = cur.gy;
        patch.width = cur.w;
        patch.height = cur.h;
        patch.xz_scale = cur.xz;
        patch.vscale = cur.vy;
        patch.bit_resolution = cur.bits;
        patch.terrain = load_patch_raw(*bytes, cur.w, cur.h, cur.vy, cur.bits);
        if (!patch.terrain.samples.empty()) {
          patches_.push_back(std::move(patch));
        }
      } else {
        std::cerr << "HeightmapCluster: missing " << cur.raw_file << '\n';
      }
      in_patch = false;
      cur.raw_file.clear();
    }
  }

  return !patches_.empty();
}

const HeightmapPatch* HeightmapCluster::patch_at(int gx, int gy) const {
  for (const auto& p : patches_) {
    if (p.grid_x == gx && p.grid_y == gy) return &p;
  }
  return nullptr;
}

void HeightmapCluster::patch_world_bounds(const HeightmapPatch& p, float& min_x, float& min_z,
                                          float& max_x, float& max_z) const {
  constexpr float kPatchSpan = 2048.f;
  min_x = static_cast<float>(p.grid_x) * kPatchSpan - kPatchSpan * 0.5f;
  max_x = min_x + kPatchSpan;
  min_z = static_cast<float>(p.grid_y) * kPatchSpan - kPatchSpan * 0.5f;
  max_z = min_z + kPatchSpan;
}

bool HeightmapCluster::world_in_cluster(float world_x, float world_z) const {
  for (const auto& p : patches_) {
    float min_x, min_z, max_x, max_z;
    patch_world_bounds(p, min_x, min_z, max_x, max_z);
    if (world_x >= min_x && world_x <= max_x && world_z >= min_z && world_z <= max_z) {
      return true;
    }
  }
  return false;
}

float HeightmapCluster::sample_height(float world_x, float world_z) const {
  const HeightmapPatch* patch = nullptr;
  float sample_x = world_x;
  float sample_z = world_z;
  for (const auto& p : patches_) {
    float min_x, min_z, max_x, max_z;
    patch_world_bounds(p, min_x, min_z, max_x, max_z);
    if (world_x < min_x || world_x > max_x || world_z < min_z || world_z > max_z) continue;
    patch = &p;
    break;
  }
  if (patch == nullptr) {
    patch = patch_at(0, 0);
    if (patch == nullptr && !patches_.empty()) patch = &patches_.front();
    if (patch == nullptr) return 0.f;
    float min_x, min_z, max_x, max_z;
    patch_world_bounds(*patch, min_x, min_z, max_x, max_z);
    sample_x = std::clamp(world_x, min_x, max_x);
    sample_z = std::clamp(world_z, min_z, max_z);
  }

  float min_x, min_z, max_x, max_z;
  patch_world_bounds(*patch, min_x, min_z, max_x, max_z);
  const float gx = (sample_x - min_x) / patch->xz_scale;
  const float gz = (sample_z - min_z) / patch->xz_scale;
  const float w = static_cast<float>(patch->width);
  const float h = static_cast<float>(patch->height);
  const float cx = std::clamp(gx, 0.f, w - 1.001f);
  const float cz = std::clamp(gz, 0.f, h - 1.001f);

  const int x0 = static_cast<int>(cx);
  const int z0 = static_cast<int>(cz);
  const int x1 = std::min(x0 + 1, static_cast<int>(patch->width) - 1);
  const int z1 = std::min(z0 + 1, static_cast<int>(patch->height) - 1);
  const float fx = cx - x0;
  const float fz = cz - z0;

  auto at = [&](int ix, int iz) {
    return patch->terrain.samples[static_cast<std::size_t>(iz) * patch->width + ix].height;
  };
  const float top = at(x0, z0) + (at(x1, z0) - at(x0, z0)) * fx;
  const float bot = at(x0, z1) + (at(x1, z1) - at(x0, z1)) * fx;
  return top + (bot - top) * fz;
}

Terrain HeightmapCluster::build_merged_terrain(float step) const {
  Terrain out;
  if (patches_.empty() || step <= 0.f) return out;

  float min_x = 1e30f, min_z = 1e30f, max_x = -1e30f, max_z = -1e30f;
  for (const auto& p : patches_) {
    float px0, pz0, px1, pz1;
    patch_world_bounds(p, px0, pz0, px1, pz1);
    min_x = std::min(min_x, px0);
    min_z = std::min(min_z, pz0);
    max_x = std::max(max_x, px1);
    max_z = std::max(max_z, pz1);
  }

  const std::uint32_t w =
      static_cast<std::uint32_t>(std::floor((max_x - min_x) / step)) + 1;
  const std::uint32_t h =
      static_cast<std::uint32_t>(std::floor((max_z - min_z) / step)) + 1;
  out.width = w;
  out.height = h;
  out.scale = 1.f;
  out.samples.resize(static_cast<std::size_t>(w) * h);

  for (std::uint32_t j = 0; j < h; ++j) {
    for (std::uint32_t i = 0; i < w; ++i) {
      const float wx = min_x + static_cast<float>(i) * step;
      const float wz = min_z + static_cast<float>(j) * step;
      out.samples[static_cast<std::size_t>(j) * w + i].height = sample_height(wx, wz);
    }
  }
  return out;
}

bool heightmap_cluster_self_test() {
  static const char* kScript = R"(
heightmapcluster.addHeightmap Heightmap 0 0
heightmap.setSize 3 3
heightmap.setScale 2/1/2
heightmap.setBitResolution 16
heightmap.loadHeightData test_primary.raw
heightmapcluster.addHeightmap Heightmap 1 0
heightmap.setSize 3 3
heightmap.setScale 8/1/8
heightmap.setBitResolution 8
heightmap.loadHeightData test_secondary.raw
)";

  // 3x3 16-bit primary: centre peak 100, edges 10.
  static const std::vector<std::uint8_t> kPrimary = {
      10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 100, 0, 10, 0,
      10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0,
  };
  static const std::vector<std::uint8_t> kSecondary(9, 30);

  HeightmapCluster cluster;
  const bool ok = cluster.load_from_script_with_reader(
      kScript, [](const std::string& name) -> std::optional<std::vector<std::uint8_t>> {
        if (name == "test_primary.raw") return kPrimary;
        if (name == "test_secondary.raw") return kSecondary;
        return std::nullopt;
      });
  if (!ok || cluster.patches().size() != 2) return false;

  const float centre = cluster.sample_height(0.f, 0.f);
  if (std::abs(centre - 100.f) > 0.5f) return false;

  // East patch centre is at world x ~ 1536m.
  const float east = cluster.sample_height(1600.f, 0.f);
  if (std::abs(east - 30.f) > 1.f) return false;

  // Far outside all patches: clamp to primary edge instead of returning 0.
  const float far_west = cluster.sample_height(-5000.f, 0.f);
  const float edge_west = cluster.sample_height(-1024.f, 0.f);
  if (std::abs(far_west - edge_west) > 1.f) return false;
  if (std::abs(far_west) < 0.01f) return false;

  const Terrain merged = cluster.build_merged_terrain(16.f);
  return merged.width > 10 && merged.height > 10;
}

}  // namespace bf2
