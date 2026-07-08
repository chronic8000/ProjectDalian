#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf2 {

struct Float3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct Aabb {
    Float3 min{};
    Float3 max{};
};

struct Matrix4 {
    float m[4][4]{};
};

struct VertexAttribute {
    std::uint16_t flag = 0;
    std::uint16_t offset = 0;
    std::uint16_t vartype = 0;
    std::uint16_t usage = 0;
};

struct Material {
    std::uint32_t alpha_mode = 0;
    std::string fx_file;
    std::string technique;
    std::vector<std::string> maps;
    std::uint32_t vertex_start = 0;
    std::uint32_t index_start = 0;
    std::uint32_t index_count = 0;
    std::uint32_t vertex_count = 0;
    Aabb bounds{};
};

struct Bone {
    std::uint32_t id = 0;
    Matrix4 transform{};
};

struct Rig {
    std::vector<Bone> bones;
};

struct Lod {
    Float3 min{};
    Float3 max{};
    Float3 pivot{};
    std::vector<Rig> rigs;
    std::vector<Matrix4> nodes;
    std::vector<Material> materials;
};

struct Geometry {
    std::vector<Lod> lods;
};

enum class MeshKind { Static, Bundled, Skinned };

struct MeshHeader {
    std::uint32_t u1 = 0;
    std::uint32_t version = 0;
    std::uint32_t u3 = 0;
    std::uint32_t u4 = 0;
    std::uint32_t u5 = 0;
};

struct Mesh {
    MeshHeader header{};
    MeshKind kind = MeshKind::Static;
    bool is_bfp4f = false;
    std::vector<Geometry> geometries;
    std::vector<VertexAttribute> vertex_attributes;
    std::uint32_t vertex_format = 0;
    std::uint32_t vertex_stride = 0;
    std::uint32_t vertex_count = 0;
    std::vector<float> vertex_data;
    std::uint32_t index_count = 0;
    std::vector<std::uint16_t> indices;
};

struct MeshInfo {
    std::string path;
    MeshKind kind = MeshKind::Static;
    std::uint32_t version = 0;
    std::uint32_t geometry_count = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material_count = 0;
    std::uint32_t lod_count = 0;
};

struct ExtractedVertex {
    Float3 position{};
    Float3 normal{};
    Float3 tangent{};
    float uv[2]{};      // channel 0: base colour
    float uv1[2]{};     // channel 1: detail
    float uv_lm[2]{};   // highest channel: baked object lightmap
};

struct ExtractedMesh {
    std::vector<ExtractedVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Material> materials;
};

// One drawable range of a textured mesh sharing the same textures.
struct TexturedSubmesh {
    std::uint32_t index_offset = 0;  // offset into TexturedMeshData::indices
    std::uint32_t index_count = 0;
    std::string base_map;    // diffuse/base color (*_c)
    std::string detail_map;  // detail (*_de)
    std::string normal_map;  // detail normal/bump (*_deb)
    std::string dirt_map;    // tiling-break dirt overlay (*_di)
    std::string crack_map;   // alpha damage decal (*_cr)
};

struct TexturedMeshData {
    std::vector<ExtractedVertex> vertices;  // uses uv (channel 0) and uv1 (channel 1)
    std::vector<std::uint32_t> indices;
    std::vector<TexturedSubmesh> submeshes;
};

class MeshLoader {
public:
    static Mesh load_from_file(const std::string& path);
    static Mesh load_from_memory(const std::vector<std::uint8_t>& data, MeshKind kind);
    static MeshInfo summarize(const Mesh& mesh);
    static ExtractedMesh extract_geometry(const Mesh& mesh, std::size_t geometry_index = 0,
                                          std::size_t lod_index = 0);
    // Extract geometry with a second UV channel and per-material submesh ranges
    // plus resolved texture map names, for textured rendering.
    static TexturedMeshData extract_textured(const Mesh& mesh, std::size_t geometry_index = 0,
                                             std::size_t lod_index = 0);
};

}  // namespace bf2
