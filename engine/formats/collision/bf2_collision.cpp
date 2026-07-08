#include "bf2_collision.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace bf2 {
namespace {

class Reader {
public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  std::uint8_t byte() {
    require(1);
    return data_[pos_++];
  }
  std::uint16_t word() {
    require(2);
    std::uint16_t v = 0;
    std::memcpy(&v, data_ + pos_, 2);
    pos_ += 2;
    return v;
  }
  std::uint32_t dword() {
    require(4);
    std::uint32_t v = 0;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
  }
  float real() {
    require(4);
    float v = 0.f;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
  }
  Float3 vec3() {
    Float3 v;
    v.x = real();
    v.y = real();
    v.z = real();
    return v;
  }
  void skip(std::size_t n) {
    require(n);
    pos_ += n;
  }
  std::size_t tell() const { return pos_; }
  std::size_t size() const { return size_; }

private:
  void require(std::size_t n) const {
    if (pos_ + n > size_) {
      throw std::runtime_error("Unexpected end of collision data");
    }
  }
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// Consume a BSP tree (we keep raw geometry, so the tree is only skipped).
void skip_bsp(Reader& r) {
  r.skip(12);  // tree min
  r.skip(12);  // tree max
  const std::uint32_t node_count = r.dword();
  for (std::uint32_t n = 0; n < node_count; ++n) {
    r.skip(4);  // split_plane_val (float)
    r.dword();  // _0x04 flags; each side is one dword regardless of leaf/subtree
    r.skip(4);  // side 0 (face_ref_start or child idx)
    r.skip(4);  // side 1
  }
  const std::uint32_t face_ref_count = r.dword();
  r.skip(static_cast<std::size_t>(face_ref_count) * 2);  // words
}

CollisionCol load_col(Reader& r, std::uint32_t v_major, std::uint32_t v_minor) {
  CollisionCol col;
  col.col_type = r.dword();

  const std::uint32_t face_count = r.dword();
  col.faces.resize(face_count);
  for (auto& face : col.faces) {
    face.v1 = r.word();
    face.v2 = r.word();
    face.v3 = r.word();
    face.material = r.word();
  }

  const std::uint32_t vert_count = r.dword();
  col.vertices.resize(vert_count);
  for (auto& v : col.vertices) {
    v = r.vec3();
  }
  col.vertex_materials.resize(vert_count);
  for (auto& m : col.vertex_materials) {
    m = r.word();
  }

  col.bounds_min = r.vec3();
  col.bounds_max = r.vec3();

  const std::uint8_t bsp_marker = r.byte();  // ASCII '0' (0x30) or '1' (0x31)
  const bool bsp_present = bsp_marker == '1';
  if (bsp_present) {
    skip_bsp(r);
  }

  // Face adjacency (debug data) only present in version >= (0,10).
  if (v_major == 0 && v_minor >= 10) {
    const std::uint32_t adj_count = r.dword();
    r.skip(static_cast<std::size_t>(adj_count) * 4);  // signed dwords
  }

  return col;
}

}  // namespace

CollisionMesh CollisionLoader::load_from_memory(const std::vector<std::uint8_t>& data) {
  Reader r(data.data(), data.size());
  CollisionMesh mesh;
  mesh.version_major = r.dword();
  mesh.version_minor = r.dword();

  const std::uint32_t geom_part_count = r.dword();
  for (std::uint32_t gp = 0; gp < geom_part_count; ++gp) {
    const std::uint32_t geom_count = r.dword();
    for (std::uint32_t g = 0; g < geom_count; ++g) {
      const std::uint32_t col_count = r.dword();
      for (std::uint32_t c = 0; c < col_count; ++c) {
        CollisionCol col = load_col(r, mesh.version_major, mesh.version_minor);

        CollisionLod lod;
        lod.type = col.col_type;
        lod.vertices = col.vertices;
        lod.faces = col.faces;
        mesh.lods.push_back(std::move(lod));

        mesh.cols.push_back(std::move(col));
      }
    }
  }

  return mesh;
}

CollisionMesh CollisionLoader::load_from_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Collision mesh not found: " + path);
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  return load_from_memory(data);
}

}  // namespace bf2
