#include "engine/formats/mesh/obj_loader.hpp"

#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace bf2 {
namespace {

struct ObjVert {
  float x = 0, y = 0, z = 0;
};
struct ObjUV {
  float u = 0, v = 0;
};
struct ObjNrm {
  float x = 0, y = 1, z = 0;
};

struct FaceIdx {
  int v = 0, vt = 0, vn = 0;
};

FaceIdx parse_face_vert(const std::string& token) {
  FaceIdx out;
  std::stringstream ss(token);
  std::string part;
  int slot = 0;
  while (std::getline(ss, part, '/')) {
    if (!part.empty()) {
      const int idx = std::atoi(part.c_str());
      if (slot == 0) out.v = idx;
      else if (slot == 1) out.vt = idx;
      else if (slot == 2) out.vn = idx;
    }
    ++slot;
  }
  return out;
}

int resolve_index(int idx, int count) {
  if (idx > 0) return idx - 1;
  if (idx < 0) return count + idx;
  return -1;
}

std::string dirname_of(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return {};
  return path.substr(0, slash + 1);
}

}  // namespace

std::unordered_map<std::string, ObjMaterial> load_obj_materials(const std::string& mtl_path) {
  std::unordered_map<std::string, ObjMaterial> out;
  std::ifstream in(mtl_path);
  if (!in) return out;
  ObjMaterial* cur = nullptr;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream ss(line);
    std::string tag;
    ss >> tag;
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (tag == "newmtl") {
      std::string name;
      ss >> name;
      cur = &out[name];
      cur->name = name;
    } else if (cur && tag == "kd") {
      ss >> cur->kd[0] >> cur->kd[1] >> cur->kd[2];
    }
  }
  return out;
}

TexturedMeshData load_obj_file(const std::string& path, const std::string& base_map) {
  TexturedMeshData out;
  std::ifstream in(path);
  if (!in) return out;

  std::vector<ObjVert> positions;
  std::vector<ObjUV> uvs;
  std::vector<ObjNrm> normals;
  positions.reserve(65536);
  uvs.reserve(65536);
  normals.reserve(65536);

  struct Key {
    int v, vt, vn;
    bool operator==(const Key& o) const { return v == o.v && vt == o.vt && vn == o.vn; }
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const {
      return (static_cast<std::size_t>(k.v) * 73856093u) ^
             (static_cast<std::size_t>(k.vt) * 19349663u) ^
             (static_cast<std::size_t>(k.vn) * 83492791u);
    }
  };
  std::unordered_map<Key, std::uint32_t, KeyHash> welded;

  auto emit_vert = [&](const FaceIdx& f) -> std::uint32_t {
    Key key{f.v, f.vt, f.vn};
    const auto it = welded.find(key);
    if (it != welded.end()) return it->second;

    ExtractedVertex ev{};
    const int vi = resolve_index(f.v, static_cast<int>(positions.size()));
    const int ti = resolve_index(f.vt, static_cast<int>(uvs.size()));
    const int ni = resolve_index(f.vn, static_cast<int>(normals.size()));
    if (vi >= 0 && vi < static_cast<int>(positions.size())) {
      ev.position = {positions[vi].x, positions[vi].y, positions[vi].z};
    }
    if (ti >= 0 && ti < static_cast<int>(uvs.size())) {
      ev.uv[0] = uvs[ti].u;
      ev.uv[1] = uvs[ti].v;
      ev.uv1[0] = uvs[ti].u;
      ev.uv1[1] = uvs[ti].v;
    }
    if (ni >= 0 && ni < static_cast<int>(normals.size())) {
      ev.normal = {normals[ni].x, normals[ni].y, normals[ni].z};
    } else {
      ev.normal = {0.f, 1.f, 0.f};
    }
    const auto id = static_cast<std::uint32_t>(out.vertices.size());
    out.vertices.push_back(ev);
    welded.emplace(key, id);
    return id;
  };

  std::string current_mtl = base_map.empty() ? "default" : base_map;
  std::vector<std::uint32_t> face_indices;
  auto flush_submesh = [&]() {
    if (face_indices.empty()) return;
    TexturedSubmesh sm;
    sm.index_offset = static_cast<std::uint32_t>(out.indices.size());
    sm.index_count = static_cast<std::uint32_t>(face_indices.size());
    sm.base_map = current_mtl;
    out.indices.insert(out.indices.end(), face_indices.begin(), face_indices.end());
    out.submeshes.push_back(std::move(sm));
    face_indices.clear();
  };

  std::string line;
  std::string mtllib;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream ss(line);
    std::string tag;
    ss >> tag;
    if (tag == "mtllib") {
      ss >> mtllib;
    } else if (tag == "v") {
      ObjVert p;
      ss >> p.x >> p.y >> p.z;
      positions.push_back(p);
    } else if (tag == "vt") {
      ObjUV t;
      ss >> t.u >> t.v;
      uvs.push_back(t);
    } else if (tag == "vn") {
      ObjNrm n;
      ss >> n.x >> n.y >> n.z;
      normals.push_back(n);
    } else if (tag == "usemtl") {
      flush_submesh();
      ss >> current_mtl;
      if (current_mtl.empty()) current_mtl = "default";
    } else if (tag == "f") {
      std::vector<FaceIdx> corners;
      std::string tok;
      while (ss >> tok) corners.push_back(parse_face_vert(tok));
      if (corners.size() < 3) continue;
      const auto i0 = emit_vert(corners[0]);
      for (std::size_t i = 1; i + 1 < corners.size(); ++i) {
        const auto i1 = emit_vert(corners[i]);
        const auto i2 = emit_vert(corners[i + 1]);
        face_indices.push_back(i0);
        face_indices.push_back(i1);
        face_indices.push_back(i2);
      }
    }
  }
  flush_submesh();

  if (out.indices.empty()) return {};

  // Flat-shade fill if normals were missing / zero
  for (std::size_t i = 0; i + 2 < out.indices.size(); i += 3) {
    auto& a = out.vertices[out.indices[i]];
    auto& b = out.vertices[out.indices[i + 1]];
    auto& c = out.vertices[out.indices[i + 2]];
    const float ax = b.position.x - a.position.x, ay = b.position.y - a.position.y,
                az = b.position.z - a.position.z;
    const float bx = c.position.x - a.position.x, by = c.position.y - a.position.y,
                bz = c.position.z - a.position.z;
    float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
      nx /= len;
      ny /= len;
      nz /= len;
    } else {
      nx = 0.f;
      ny = 1.f;
      nz = 0.f;
    }
    auto fix = [&](ExtractedVertex& v) {
      if (std::fabs(v.normal.x) + std::fabs(v.normal.y) + std::fabs(v.normal.z) < 1e-5f) {
        v.normal = {nx, ny, nz};
      }
    };
    fix(a);
    fix(b);
    fix(c);
  }

  if (out.submeshes.empty()) {
    TexturedSubmesh sm;
    sm.index_offset = 0;
    sm.index_count = static_cast<std::uint32_t>(out.indices.size());
    sm.base_map = base_map.empty() ? "default" : base_map;
    out.submeshes.push_back(std::move(sm));
  }

  (void)mtllib;
  (void)dirname_of;
  return out;
}

}  // namespace bf2
