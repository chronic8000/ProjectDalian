#include "overgrowth_instances.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <unordered_set>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

float og_hash(int x, int y, int salt) {
  std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                    static_cast<std::uint32_t>(y) * 668265263u +
                    static_cast<std::uint32_t>(salt) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xFFFFFF) / static_cast<float>(0x1000000);
}


}  // namespace

std::string resolve_overgrowth_mesh(ResourceManager& resources, const std::string& geometry) {
  static std::unordered_map<std::string, std::string> cache;
  const std::string key = lower(geometry);
  if (const auto it = cache.find(key); it != cache.end()) return it->second;

  const char* exts[] = {".bundledmesh", ".staticmesh"};
  std::string best;
  for (const auto& path : resources.archives().list()) {
    for (const char* ext : exts) {
      const std::string suffix = key + ext;
      if (path.size() >= suffix.size() &&
          path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        best = path;
        break;
      }
    }
    if (!best.empty()) break;
  }
  cache[key] = best;
  return best;
}

std::vector<OvergrowthInstance> build_overgrowth_instances(const OvergrowthParser& defs,
                                                           const std::vector<std::uint8_t>& raw,
                                                           ResourceManager& resources,
                                                           float cell_size,
                                                           std::size_t max_instances) {
  std::vector<OvergrowthInstance> out;
  if (raw.empty() || defs.materials().empty()) return out;

  const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(raw.size()))));
  if (side <= 0 || static_cast<std::size_t>(side) * side != raw.size()) return out;

  std::unordered_map<std::string, std::string> mesh_by_geom;
  std::unordered_map<int, std::vector<const OvergrowthTypeDef*>> types_by_mat;
  for (const auto& mat : defs.materials()) {
    auto& list = types_by_mat[mat.id];
    for (const auto& tn : mat.type_names) {
      const auto it = defs.types().find(tn);
      if (it == defs.types().end()) continue;
      list.push_back(&it->second);
      if (mesh_by_geom.find(it->second.geometry) == mesh_by_geom.end()) {
        mesh_by_geom[it->second.geometry] = resolve_overgrowth_mesh(resources, it->second.geometry);
      }
    }
  }

  const float cell_area = cell_size * cell_size;
  out.reserve(std::min(max_instances, static_cast<std::size_t>(side) * side / 4));

  for (int j = 0; j < side && out.size() < max_instances; ++j) {
    for (int i = 0; i < side && out.size() < max_instances; ++i) {
      const int id = raw[static_cast<std::size_t>(j) * side + i];
      if (id <= 0) continue;
      const auto tit = types_by_mat.find(id);
      if (tit == types_by_mat.end() || tit->second.empty()) continue;

      const float wx =
          (static_cast<float>(i) - (side - 1) * 0.5f) * cell_size + (og_hash(i, j, 1) - 0.5f) * cell_size;
      const float wz =
          (static_cast<float>(j) - (side - 1) * 0.5f) * cell_size + (og_hash(i, j, 2) - 0.5f) * cell_size;

      // Pick a type from this material.
      const auto& types = tit->second;
      const auto* type = types[static_cast<std::size_t>(og_hash(i, j, 3) * types.size()) % types.size()];
      if (!type || type->density <= 0.f) continue;

      const float chance = std::min(1.f, type->density * cell_area * 0.04f);
      if (og_hash(i, j, 4) > chance) continue;

      const auto mit = mesh_by_geom.find(type->geometry);
      if (mit == mesh_by_geom.end() || mit->second.empty()) continue;

      OvergrowthInstance inst;
      inst.mesh_vpath = mit->second;
      inst.position[0] = wx;
      inst.position[1] = 0.f;  // snapped at placement time
      inst.position[2] = wz;
      inst.rotation[0] = og_hash(i, j, 5) * 360.f;
      inst.rotation[1] = 0.f;
      inst.rotation[2] = 0.f;
      inst.scale = 0.85f + og_hash(i, j, 6) * 0.3f;
      out.push_back(inst);
    }
  }
  return out;
}

bool overgrowth_instances_self_test() {
  static const char* kCon = R"(
Overgrowth.addMaterial trees 3
Overgrowth.setActiveMaterial trees
Overgrowth.addType t1
Overgrowth.setActiveType t1
OvergrowthType.geometry test_tree
OvergrowthType.density 10
)";
  OvergrowthParser parser;
  parser.parse(kCon);

  std::vector<std::uint8_t> raw(9, 0);
  raw[4] = 3;  // centre cell

  // No archives mounted — mesh resolution fails, but we still get zero instances gracefully.
  ResourceManager resources;
  const auto inst = build_overgrowth_instances(parser, raw, resources, 2.f, 100);
  return inst.empty();  // expected without mesh archive
}

}  // namespace bf2
