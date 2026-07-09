#pragma once

// Parses BF2 Undergrowth (grass) data: Undergrowth.cfg describes per-material
// grass types (atlas texture + density), Undergrowth.raw is a WxH map of material
// ids across the terrain, and UndergrowthAtlas.tai gives each grass texture's
// sub-rect inside UndergrowthAtlas0.dds. Together these let us scatter grass
// billboards on the ground matching the map's authored vegetation.

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

struct GrassType {
  glm::vec4 atlas_rect{0, 0, 1, 1};  // u0, v0, uw, vh in the atlas
  float density = 1.0f;              // blades per square metre (roughly)
};

struct Undergrowth {
  int width = 0;
  int height = 0;
  float cell_size = 2.0f;
  bool centered = true;
  std::vector<std::uint8_t> ids;              // material id per texel (row-major, row = z)
  std::unordered_map<int, GrassType> grass;   // material id -> grass (first type)

  bool valid() const { return width > 0 && !ids.empty() && !grass.empty(); }

  int id_at_grid(int i, int j) const {
    if (i < 0 || j < 0 || i >= width || j >= height) return -1;
    return ids[static_cast<std::size_t>(j) * width + i];
  }

  // World XZ -> grass type for that spot (nullptr if bare ground).
  const GrassType* grass_at(float wx, float wz) const {
    const int i = static_cast<int>(std::lround(wx / cell_size + (centered ? width * 0.5f : 0.f)));
    const int j = static_cast<int>(std::lround(wz / cell_size + (centered ? height * 0.5f : 0.f)));
    const int id = id_at_grid(i, j);
    if (id < 0) return nullptr;
    const auto it = grass.find(id);
    return it == grass.end() ? nullptr : &it->second;
  }
};

namespace detail {

inline std::string ug_basename(std::string s) {
  const auto slash = s.find_last_of("/\\");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const auto dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace detail

// tai: texture basename (lower) -> atlas rect (u0,v0,uw,vh).
inline std::unordered_map<std::string, glm::vec4> parse_undergrowth_atlas(const std::string& tai) {
  std::unordered_map<std::string, glm::vec4> out;
  std::istringstream lines(tai);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find_first_of(" \t");
    if (tab == std::string::npos) continue;
    const std::string key = detail::ug_basename(line.substr(0, tab));
    const auto comma = line.find(',');
    if (comma == std::string::npos) continue;
    std::string nums = line.substr(comma + 1);
    for (char& c : nums)
      if (c == ',') c = ' ';
    std::istringstream ns(nums);
    int idx = 0;
    float woff = 0, hoff = 0, w = 1, h = 1;
    if (!(ns >> idx >> woff >> hoff >> w >> h)) continue;
    out[key] = glm::vec4(woff, hoff, w, h);
  }
  return out;
}

inline Undergrowth parse_undergrowth(const std::string& cfg, const std::vector<std::uint8_t>& raw,
                                     const std::string& tai, float cell_size) {
  Undergrowth ug;
  ug.cell_size = cell_size;
  const auto side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(raw.size()))));
  if (side > 0 && static_cast<std::size_t>(side) * side == raw.size()) {
    ug.width = ug.height = side;
    ug.ids = raw;
  }

  const auto atlas = parse_undergrowth_atlas(tai);

  // Walk the .cfg: "Material <name> <id> { ... Type <name> { Texture <t> Density <d> } }".
  std::istringstream lines(cfg);
  std::string line;
  int cur_id = -1;
  bool have_type = false;
  std::string cur_tex;
  float cur_density = 1.0f;
  auto commit_type = [&]() {
    if (cur_id >= 0 && have_type && !cur_tex.empty() && ug.grass.find(cur_id) == ug.grass.end()) {
      // Only register grass whose texture has a known atlas sub-rect. Without it
      // the billboard would sample the whole atlas (default 0,0,1,1) and show up
      // as a tiny crossed square of the entire grass sheet, not a blade.
      const auto it = atlas.find(detail::ug_basename(cur_tex));
      if (it != atlas.end()) {
        GrassType g;
        g.density = cur_density;
        g.atlas_rect = it->second;
        ug.grass[cur_id] = g;
      }
    }
    have_type = false;
    cur_tex.clear();
    cur_density = 1.0f;
  };
  while (std::getline(lines, line)) {
    std::istringstream ls(line);
    std::string tok;
    if (!(ls >> tok)) continue;
    if (tok == "Material") {
      commit_type();
      std::string name;
      int id = -1;
      ls >> name >> id;
      cur_id = id;
    } else if (tok == "Type") {
      commit_type();
      have_type = true;
    } else if (tok == "Texture") {
      ls >> cur_tex;
    } else if (tok == "Density") {
      ls >> cur_density;
    }
  }
  commit_type();
  return ug;
}

namespace detail {

inline float ug_hash(int x, int y, int salt) {
  std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                    static_cast<std::uint32_t>(y) * 668265263u +
                    static_cast<std::uint32_t>(salt) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xFFFFFF) / static_cast<float>(0x1000000);  // [0,1)
}

// Emit one alpha-tested quad (6 verts * 6 floats) into out.
inline void ug_emit_quad(std::vector<float>& out, float bx, float by, float bz, float dx, float dz,
                         float half_w, float h, const glm::vec4& rect) {
  const float u0 = rect.x, v0 = rect.y, uw = rect.z, vh = rect.w;
  const float lx = bx - dx * half_w, lz = bz - dz * half_w;
  const float rx = bx + dx * half_w, rz = bz + dz * half_w;
  // base-left, base-right, top-right, top-left  (v0 = top of texture)
  const float bl[6] = {lx, by, lz, u0, v0 + vh, 0.f};
  const float br[6] = {rx, by, rz, u0 + uw, v0 + vh, 0.f};
  const float tr[6] = {rx, by + h, rz, u0 + uw, v0, 1.f};
  const float tl[6] = {lx, by + h, lz, u0, v0, 1.f};
  auto push = [&](const float* v) { out.insert(out.end(), v, v + 6); };
  push(bl); push(br); push(tr);
  push(bl); push(tr); push(tl);
}

}  // namespace detail

// Build crossed-billboard grass geometry (6 floats/vertex: pos.xyz, uv.xy, sway)
// for blades within `radius` of (cx,cz). height(x,z) returns ground Y. Placement
// is deterministic per grid cell so blades stay put as the camera moves.
template <class HeightFn>
inline void build_grass_vertices(const Undergrowth& ug, HeightFn&& height, float cx, float cz,
                                 float radius, float water_level, std::vector<float>& out) {
  out.clear();
  if (!ug.valid()) return;
  const float spacing = 0.7f;
  const float cell_area = spacing * spacing;
  const int i0 = static_cast<int>(std::floor((cx - radius) / spacing));
  const int i1 = static_cast<int>(std::ceil((cx + radius) / spacing));
  const int j0 = static_cast<int>(std::floor((cz - radius) / spacing));
  const int j1 = static_cast<int>(std::ceil((cz + radius) / spacing));
  const float r2 = radius * radius;
  for (int j = j0; j <= j1; ++j) {
    for (int i = i0; i <= i1; ++i) {
      const float jx = (detail::ug_hash(i, j, 1) - 0.5f) * spacing;
      const float jz = (detail::ug_hash(i, j, 2) - 0.5f) * spacing;
      const float wx = i * spacing + jx;
      const float wz = j * spacing + jz;
      const float dxr = wx - cx, dzr = wz - cz;
      if (dxr * dxr + dzr * dzr > r2) continue;
      const GrassType* g = ug.grass_at(wx, wz);
      if (!g) continue;
      // Probabilistic thinning by density (blades per m^2).
      if (detail::ug_hash(i, j, 3) > std::min(1.0f, g->density * cell_area)) continue;
      const float gy = height(wx, wz);
      if (gy <= water_level + 0.1f) continue;  // no grass in the sea
      const float ang = detail::ug_hash(i, j, 4) * 6.2831853f;
      const float sz = 0.55f + detail::ug_hash(i, j, 5) * 0.6f;
      const float half_w = 0.5f * sz;
      const float h = 0.7f * sz;
      const float dx = std::cos(ang), dz = std::sin(ang);
      detail::ug_emit_quad(out, wx, gy, wz, dx, dz, half_w, h, g->atlas_rect);
      detail::ug_emit_quad(out, wx, gy, wz, -dz, dx, half_w, h, g->atlas_rect);
    }
  }
}

}  // namespace bf2
