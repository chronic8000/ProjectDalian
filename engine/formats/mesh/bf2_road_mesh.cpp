#include "bf2_road_mesh.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace bf2 {
namespace {

constexpr std::uint32_t kRoadVer14 = 0x00010004;
constexpr std::uint32_t kRoadVer12 = 0x00010002;
constexpr std::size_t kHeaderBytes = 52;
constexpr std::size_t kVertexStride = 32;

Float3 read_f3(const std::uint8_t* p) {
  Float3 v;
  std::memcpy(&v.x, p, 4);
  std::memcpy(&v.y, p + 4, 4);
  std::memcpy(&v.z, p + 8, 4);
  return v;
}

float read_f(const std::uint8_t* p) {
  float v = 0.f;
  std::memcpy(&v, p, 4);
  return v;
}

std::uint32_t read_u32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  std::memcpy(&v, p, 4);
  return v;
}

std::uint16_t read_u16(const std::uint8_t* p) {
  std::uint16_t v = 0;
  std::memcpy(&v, p, 2);
  return v;
}

}  // namespace

bool is_road_compiled_bytes(const std::vector<std::uint8_t>& data) {
  if (data.size() < kHeaderBytes) return false;
  const std::uint32_t ver = read_u32(data.data());
  return ver == kRoadVer14 || ver == kRoadVer12;
}

RoadCompiledMesh load_road_compiled(const std::vector<std::uint8_t>& data) {
  if (!is_road_compiled_bytes(data)) {
    throw std::runtime_error("Not a BF2 RoadCompiled mesh");
  }
  RoadCompiledMesh road;
  road.version = read_u32(data.data());
  road.start = read_f3(data.data() + 4);
  road.length = read_f(data.data() + 16);
  road.end = read_f3(data.data() + 20);
  road.misc = read_f3(data.data() + 32);
  // offset 44: unused/zero
  const std::uint32_t nv = read_u32(data.data() + 48);
  const std::size_t verts_bytes = static_cast<std::size_t>(nv) * kVertexStride;
  if (kHeaderBytes + verts_bytes + 4 > data.size()) {
    throw std::runtime_error("RoadCompiled vertex block truncated");
  }

  road.positions.resize(nv);
  road.u.resize(nv);
  road.v.resize(nv);
  road.u1.resize(nv);
  road.v1.resize(nv);
  road.alpha.resize(nv);
  for (std::uint32_t i = 0; i < nv; ++i) {
    const std::uint8_t* vp = data.data() + kHeaderBytes + i * kVertexStride;
    // RoadCompiled.fx APP2VS: Pos | Tex0 | Tex1 | Alpha  (32 bytes)
    road.positions[i] = read_f3(vp);
    road.u[i] = read_f(vp + 12);      // Tex0.u across width [0,1]
    road.v[i] = read_f(vp + 16);      // Tex0.v along road
    road.u1[i] = read_f(vp + 20);     // Tex1.u
    road.v1[i] = read_f(vp + 24);     // Tex1.v
    road.alpha[i] = read_f(vp + 28);  // fade
  }

  const std::size_t ioff = kHeaderBytes + verts_bytes;
  const std::uint32_t nidx = read_u32(data.data() + ioff);
  if (ioff + 4 + static_cast<std::size_t>(nidx) * 2 > data.size()) {
    throw std::runtime_error("RoadCompiled index block truncated");
  }
  road.indices.resize(nidx);
  for (std::uint32_t i = 0; i < nidx; ++i) {
    road.indices[i] = read_u16(data.data() + ioff + 4 + i * 2);
  }
  return road;
}

TexturedMeshData road_compiled_to_textured(const RoadCompiledMesh& road,
                                           const std::string& base_map,
                                           const std::string& detail_map) {
  TexturedMeshData out;
  if (road.positions.empty() || road.indices.size() < 3) return out;

  out.vertices.resize(road.positions.size());
  for (std::size_t i = 0; i < road.positions.size(); ++i) {
    ExtractedVertex v;
    v.position = road.positions[i];
    // Slight lift so the ribbon wins depth against coplanar terrain (avoids the
    // brown/black z-fight stripes on Dalian dirt roads).
    v.position.y += 0.06f;
    v.normal = {0.f, 1.f, 0.f};
    // Surface map uses Tex0 (CLAMP U / WRAP V in BF2).
    v.uv[0] = road.u[i];
    v.uv[1] = road.v[i];
    // Detail map uses Tex1; RoadCompiled.fx samples Tex1*0.1.
    if (i < road.u1.size() && i < road.v1.size()) {
      v.uv1[0] = road.u1[i] * 0.1f;
      v.uv1[1] = road.v1[i] * 0.1f;
    } else {
      v.uv1[0] = v.uv[0];
      v.uv1[1] = v.uv[1];
    }
    // RoadCompiled vertex Alpha → soft edge fade into terrain (dirt blend).
    // Packed into unused lightmap UV.x; alpha_mode 3 reads it in the shader.
    v.uv_lm[0] = (i < road.alpha.size()) ? road.alpha[i] : 1.f;
    v.uv_lm[1] = 0.f;
    out.vertices[i] = v;
  }

  // Recompute smooth-ish normals from triangles.
  for (std::size_t t = 0; t + 2 < road.indices.size(); t += 3) {
    const std::uint16_t i0 = road.indices[t];
    const std::uint16_t i1 = road.indices[t + 1];
    const std::uint16_t i2 = road.indices[t + 2];
    if (i0 >= out.vertices.size() || i1 >= out.vertices.size() || i2 >= out.vertices.size()) {
      continue;
    }
    const Float3& a = out.vertices[i0].position;
    const Float3& b = out.vertices[i1].position;
    const Float3& c = out.vertices[i2].position;
    const Float3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const Float3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
    Float3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    if (n.y < 0.f) {
      n.x = -n.x;
      n.y = -n.y;
      n.z = -n.z;
    }
    for (std::uint16_t ix : {i0, i1, i2}) {
      out.vertices[ix].normal.x += n.x;
      out.vertices[ix].normal.y += n.y;
      out.vertices[ix].normal.z += n.z;
    }
  }
  for (auto& v : out.vertices) {
    const float len =
        std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z);
    if (len > 1e-6f) {
      v.normal.x /= len;
      v.normal.y /= len;
      v.normal.z /= len;
    } else {
      v.normal = {0.f, 1.f, 0.f};
    }
  }

  out.indices.assign(road.indices.begin(), road.indices.end());

  TexturedSubmesh sub;
  sub.index_offset = 0;
  sub.index_count = static_cast<std::uint32_t>(out.indices.size());
  sub.base_map = base_map;
  sub.detail_map = detail_map;
  out.submeshes.push_back(std::move(sub));
  return out;
}

std::vector<Float3> road_compiled_collision_soup(const RoadCompiledMesh& road) {
  std::vector<Float3> out;
  out.reserve(road.indices.size());
  for (std::size_t t = 0; t + 2 < road.indices.size(); t += 3) {
    const std::uint16_t i0 = road.indices[t];
    const std::uint16_t i1 = road.indices[t + 1];
    const std::uint16_t i2 = road.indices[t + 2];
    if (i0 >= road.positions.size() || i1 >= road.positions.size() ||
        i2 >= road.positions.size()) {
      continue;
    }
    out.push_back(road.positions[i0]);
    out.push_back(road.positions[i1]);
    out.push_back(road.positions[i2]);
  }
  return out;
}

bool road_compiled_self_test() {
  // Minimal synthetic road: version, start/len/end/misc/zero/nv=3, 3 verts, nidx=3.
  std::vector<std::uint8_t> bytes(kHeaderBytes + 3 * kVertexStride + 4 + 6, 0);
  const std::uint32_t ver = kRoadVer14;
  std::memcpy(bytes.data(), &ver, 4);
  float start[3] = {10.f, 20.f, 30.f};
  std::memcpy(bytes.data() + 4, start, 12);
  float len = 5.f;
  std::memcpy(bytes.data() + 16, &len, 4);
  const std::uint32_t nv = 3;
  std::memcpy(bytes.data() + 48, &nv, 4);
  for (int i = 0; i < 3; ++i) {
    float pos[3] = {static_cast<float>(i), 0.f, 0.f};
    std::memcpy(bytes.data() + kHeaderBytes + i * kVertexStride, pos, 12);
    // Tex0.u across, Tex0.v along, Tex1, Alpha
    float t0u = static_cast<float>(i) * 0.5f;
    float t0v = 0.25f;
    float t1u = 0.1f;
    float t1v = 1.0f;
    float alpha = 1.f;
    std::memcpy(bytes.data() + kHeaderBytes + i * kVertexStride + 12, &t0u, 4);
    std::memcpy(bytes.data() + kHeaderBytes + i * kVertexStride + 16, &t0v, 4);
    std::memcpy(bytes.data() + kHeaderBytes + i * kVertexStride + 20, &t1u, 4);
    std::memcpy(bytes.data() + kHeaderBytes + i * kVertexStride + 24, &t1v, 4);
    std::memcpy(bytes.data() + kHeaderBytes + i * kVertexStride + 28, &alpha, 4);
  }
  const std::uint32_t nidx = 3;
  const std::size_t ioff = kHeaderBytes + 3 * kVertexStride;
  std::memcpy(bytes.data() + ioff, &nidx, 4);
  const std::uint16_t idx[3] = {0, 1, 2};
  std::memcpy(bytes.data() + ioff + 4, idx, 6);

  if (!is_road_compiled_bytes(bytes)) return false;
  const auto road = load_road_compiled(bytes);
  if (road.positions.size() != 3 || road.indices.size() != 3) return false;
  if (road.start.x != 10.f || road.positions[1].x != 1.f) return false;
  if (road.u[2] != 1.f || road.v[0] != 0.25f || road.alpha[0] != 1.f) return false;
  const auto tex = road_compiled_to_textured(road, "roads/textures/test.dds", {});
  if (tex.vertices.size() != 3 || tex.indices.size() != 3) return false;
  if (tex.vertices[2].uv[0] != 1.f) return false;
  if (tex.vertices[0].uv_lm[0] != 1.f) return false;
  const auto soup = road_compiled_collision_soup(road);
  if (soup.size() != 3) return false;
  return true;
}

}  // namespace bf2
