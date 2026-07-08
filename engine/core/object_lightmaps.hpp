#pragma once

// Parses BF2's per-instance object lightmap index (Lightmaps/Objects/
// LightmapAtlas.tai). Each placed static object gets a baked lightmap packed
// into one of the LightmapAtlasN.dds atlases; the .tai maps an instance
// (identified by "<meshname>=<part>=<x>=<y>=<z>") to its atlas index and the
// UV sub-rect (offset + scale) the object's lightmap UV should sample.

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

struct ObjLmEntry {
  glm::vec3 pos{};         // rounded world position from the .tai key
  int atlas = 0;           // LightmapAtlas<atlas>.dds
  glm::vec4 xform{1, 1, 0, 0};  // xy = uv scale (w/h), zw = uv offset (woff/hoff)
};

struct ObjectLightmaps {
  std::unordered_map<std::string, std::vector<ObjLmEntry>> by_name;
  int atlas_count = 0;

  // Nearest lightmap for a mesh (base name, lower-case) at world position p.
  const ObjLmEntry* find(const std::string& mesh_lower, const glm::vec3& p,
                         float tol = 3.0f) const {
    const auto it = by_name.find(mesh_lower);
    if (it == by_name.end()) return nullptr;
    const ObjLmEntry* best = nullptr;
    float best_d2 = tol * tol;
    for (const auto& e : it->second) {
      const glm::vec3 d = e.pos - p;
      const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
      if (d2 <= best_d2) {
        best_d2 = d2;
        best = &e;
      }
    }
    return best;
  }
};

namespace detail {

inline std::string basename_no_ext(std::string s) {
  const auto slash = s.find_last_of("/\\");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const auto dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace detail

inline ObjectLightmaps parse_object_lightmaps(const std::string& tai) {
  ObjectLightmaps out;
  std::istringstream lines(tai);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty() || line[0] == '#') continue;
    // Left side (up to first tab/space run) is the per-instance lightmap name;
    // right side is "<atlas file>, idx, woff, hoff, w, h".
    const auto tab = line.find_first_of(" \t");
    if (tab == std::string::npos) continue;
    const std::string left = line.substr(0, tab);
    std::string right = line.substr(tab);

    // Key: <meshname>=<part>=<x>=<y>=<z>
    std::string key = detail::basename_no_ext(left);
    std::vector<std::string> parts;
    std::string cur;
    for (char c : key) {
      if (c == '=') {
        parts.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    parts.push_back(cur);
    if (parts.size() < 5) continue;  // need name, part, x, y, z

    ObjLmEntry e;
    try {
      e.pos = glm::vec3(std::stof(parts[parts.size() - 3]), std::stof(parts[parts.size() - 2]),
                        std::stof(parts[parts.size() - 1]));
    } catch (...) {
      continue;
    }
    const std::string mesh_name = parts[0];
    const std::string part = parts[parts.size() - 4];

    // Only take the primary lightmap part; multi-part objects (carriers, etc.)
    // are rare and the extra parts would need per-submesh UV routing.
    if (part != "00" && part != "0") continue;

    // Right side: skip the atlas filename token up to the first comma.
    const auto comma = right.find(',');
    if (comma == std::string::npos) continue;
    std::string nums = right.substr(comma + 1);
    for (char& c : nums) {
      if (c == ',') c = ' ';
    }
    std::istringstream ns(nums);
    float woff = 0, hoff = 0, w = 1, h = 1;
    int idx = 0;
    if (!(ns >> idx >> woff >> hoff >> w >> h)) continue;
    e.atlas = idx;
    e.xform = glm::vec4(w, h, woff, hoff);
    out.atlas_count = std::max(out.atlas_count, idx + 1);
    out.by_name[mesh_name].push_back(e);
  }
  return out;
}

}  // namespace bf2
