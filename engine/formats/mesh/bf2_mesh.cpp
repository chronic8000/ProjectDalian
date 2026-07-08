#include "bf2_mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bf2 {
namespace {

std::string extension_lower(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

MeshKind detect_kind(const std::string& path) {
    const auto ext = extension_lower(path);
    if (ext == "bundledmesh") {
        return MeshKind::Bundled;
    }
    if (ext == "skinnedmesh") {
        return MeshKind::Skinned;
    }
    return MeshKind::Static;
}

std::string read_string(std::istream& in) {
    std::uint32_t length = 0;
    in.read(reinterpret_cast<char*>(&length), sizeof(length));
    if (!in) {
        throw std::runtime_error("Failed to read string length");
    }
    if (length == 0) {
        return {};
    }
    std::string value(length, '\0');
    in.read(value.data(), static_cast<std::streamsize>(length));
    if (!in) {
        throw std::runtime_error("Failed to read string payload");
    }
    return value;
}

void read_exact(std::istream& in, void* dst, std::size_t size) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("Unexpected end of mesh file");
    }
}

Material read_material(std::istream& in, std::uint32_t version, MeshKind kind) {
    Material material;
    if (kind != MeshKind::Skinned) {
        read_exact(in, &material.alpha_mode, sizeof(material.alpha_mode));
    }
    material.fx_file = read_string(in);
    material.technique = read_string(in);

    std::uint32_t map_count = 0;
    read_exact(in, &map_count, sizeof(map_count));
    material.maps.reserve(map_count);
    for (std::uint32_t i = 0; i < map_count; ++i) {
        material.maps.push_back(read_string(in));
    }

    read_exact(in, &material.vertex_start, sizeof(material.vertex_start));
    read_exact(in, &material.index_start, sizeof(material.index_start));
    read_exact(in, &material.index_count, sizeof(material.index_count));
    read_exact(in, &material.vertex_count, sizeof(material.vertex_count));

    std::uint32_t u4 = 0;
    std::uint16_t u5 = 0;
    std::uint16_t u6 = 0;
    read_exact(in, &u4, sizeof(u4));
    read_exact(in, &u5, sizeof(u5));
    read_exact(in, &u6, sizeof(u6));

    if (kind != MeshKind::Skinned && version == 11) {
        read_exact(in, &material.bounds, sizeof(material.bounds));
    }
    return material;
}

Rig read_rig(std::istream& in) {
    Rig rig;
    std::uint32_t bone_count = 0;
    read_exact(in, &bone_count, sizeof(bone_count));
    rig.bones.resize(bone_count);
    if (bone_count > 0) {
        read_exact(in, rig.bones.data(), bone_count * sizeof(Bone));
    }
    return rig;
}

void read_node_data(std::istream& in, Lod& lod, std::uint32_t version, MeshKind kind) {
    read_exact(in, &lod.min, sizeof(lod.min));
    read_exact(in, &lod.max, sizeof(lod.max));
    if (version <= 6) {
        read_exact(in, &lod.pivot, sizeof(lod.pivot));
    }

    if (kind == MeshKind::Skinned) {
        std::uint32_t rig_count = 0;
        read_exact(in, &rig_count, sizeof(rig_count));
        lod.rigs.resize(rig_count);
        for (std::uint32_t i = 0; i < rig_count; ++i) {
            lod.rigs[i] = read_rig(in);
        }
    } else {
        std::uint32_t node_count = 0;
        read_exact(in, &node_count, sizeof(node_count));
        if (kind != MeshKind::Bundled && node_count > 0) {
            lod.nodes.resize(node_count);
            read_exact(in, lod.nodes.data(), node_count * sizeof(Matrix4));
        }
    }
}

void read_material_data(std::istream& in, Lod& lod, std::uint32_t version, MeshKind kind) {
    std::uint32_t material_count = 0;
    read_exact(in, &material_count, sizeof(material_count));
    lod.materials.resize(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i) {
        lod.materials[i] = read_material(in, version, kind);
    }
}

Geometry read_geometry(std::istream& in) {
    Geometry geometry;
    std::uint32_t lod_count = 0;
    read_exact(in, &lod_count, sizeof(lod_count));
    geometry.lods.resize(lod_count);
    return geometry;
}

Mesh parse_mesh(std::istream& in, MeshKind kind) {
    Mesh mesh;
    mesh.kind = kind;

    read_exact(in, &mesh.header, sizeof(mesh.header));

    std::uint8_t game_marker = 0;
    read_exact(in, &game_marker, sizeof(game_marker));
    mesh.is_bfp4f = game_marker == 1;

    std::uint32_t geometry_count = 0;
    read_exact(in, &geometry_count, sizeof(geometry_count));
    mesh.geometries.resize(geometry_count);
    for (auto& geometry : mesh.geometries) {
        geometry = read_geometry(in);
    }

    std::uint32_t attribute_count = 0;
    read_exact(in, &attribute_count, sizeof(attribute_count));
    mesh.vertex_attributes.resize(attribute_count);
    if (attribute_count > 0) {
        read_exact(in, mesh.vertex_attributes.data(), attribute_count * sizeof(VertexAttribute));
    }

    read_exact(in, &mesh.vertex_format, sizeof(mesh.vertex_format));
    read_exact(in, &mesh.vertex_stride, sizeof(mesh.vertex_stride));
    read_exact(in, &mesh.vertex_count, sizeof(mesh.vertex_count));

    const std::size_t floats_per_vertex =
        mesh.vertex_stride > 0 ? mesh.vertex_stride / std::max<std::uint32_t>(mesh.vertex_format, 1u) : 0;
    mesh.vertex_data.resize(static_cast<std::size_t>(mesh.vertex_count) * floats_per_vertex);
    if (!mesh.vertex_data.empty()) {
        read_exact(in, mesh.vertex_data.data(), mesh.vertex_count * mesh.vertex_stride);
    }

    read_exact(in, &mesh.index_count, sizeof(mesh.index_count));
    mesh.indices.resize(mesh.index_count);
    if (mesh.index_count > 0) {
        read_exact(in, mesh.indices.data(), mesh.index_count * sizeof(std::uint16_t));
    }

    if (mesh.kind != MeshKind::Skinned) {
        std::uint32_t u2 = 0;
        read_exact(in, &u2, sizeof(u2));
    }

    for (auto& geometry : mesh.geometries) {
        for (auto& lod : geometry.lods) {
            read_node_data(in, lod, mesh.header.version, mesh.kind);
        }
    }

    for (auto& geometry : mesh.geometries) {
        for (auto& lod : geometry.lods) {
            read_material_data(in, lod, mesh.header.version, mesh.kind);
        }
    }

    return mesh;
}

}  // namespace

Mesh MeshLoader::load_from_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Mesh file not found: " + path);
    }
    return parse_mesh(in, detect_kind(path));
}

Mesh MeshLoader::load_from_memory(const std::vector<std::uint8_t>& data, MeshKind kind) {
    std::string buffer(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream in(buffer, std::ios::binary);
    return parse_mesh(in, kind);
}

MeshInfo MeshLoader::summarize(const Mesh& mesh) {
    MeshInfo info;
    info.kind = mesh.kind;
    info.version = mesh.header.version;
    info.geometry_count = static_cast<std::uint32_t>(mesh.geometries.size());
    info.vertex_count = mesh.vertex_count;
    info.index_count = mesh.index_count;

    for (const auto& geometry : mesh.geometries) {
        info.lod_count += static_cast<std::uint32_t>(geometry.lods.size());
        for (const auto& lod : geometry.lods) {
            info.material_count += static_cast<std::uint32_t>(lod.materials.size());
        }
    }
    return info;
}

ExtractedMesh MeshLoader::extract_geometry(const Mesh& mesh, std::size_t geometry_index,
                                             std::size_t lod_index) {
    if (geometry_index >= mesh.geometries.size()) {
        throw std::out_of_range("geometry index out of range");
    }
    const auto& geometry = mesh.geometries[geometry_index];
    if (lod_index >= geometry.lods.size()) {
        throw std::out_of_range("lod index out of range");
    }
    const auto& lod = geometry.lods[lod_index];

    ExtractedMesh extracted;
    extracted.materials = lod.materials;

  std::uint16_t position_offset = 0;
  std::uint16_t normal_offset = 0;
  std::uint16_t uv_offset = 0;
  for (const auto& attr : mesh.vertex_attributes) {
    if (attr.flag != 0) {
      continue;
    }
    if (attr.usage == 0) {
      position_offset = attr.offset;
    } else if (attr.usage == 3) {
      normal_offset = attr.offset;
    } else if (attr.usage == 6) {
      /* tangent channel available for future skinned meshes */
    } else if (attr.usage == 5 || attr.usage == 0x101 || attr.usage == 0x105) {
      if (uv_offset == 0) {
        uv_offset = attr.offset;
      }
    }
  }

  const auto floats_per_vertex =
      mesh.vertex_stride / std::max<std::uint32_t>(mesh.vertex_format, 1u);
  const auto max_float_index = mesh.vertex_data.size();

  std::uint32_t max_vertex = mesh.vertex_count;
  for (const auto& material : lod.materials) {
    max_vertex = std::max(max_vertex, material.vertex_start + material.vertex_count);
  }
  max_vertex = std::min(max_vertex, mesh.vertex_count);
  extracted.vertices.resize(max_vertex);

  for (std::uint32_t i = 0; i < max_vertex; ++i) {
    ExtractedVertex vertex;
    const std::size_t base =
        static_cast<std::size_t>(i) * floats_per_vertex + position_offset / sizeof(float);
    if (base + 2 < max_float_index) {
      vertex.position.x = mesh.vertex_data[base];
      vertex.position.y = mesh.vertex_data[base + 1];
      vertex.position.z = mesh.vertex_data[base + 2];
    }
    const std::size_t normal_base =
        static_cast<std::size_t>(i) * floats_per_vertex + normal_offset / sizeof(float);
    if (normal_base + 2 < max_float_index) {
      vertex.normal.x = mesh.vertex_data[normal_base];
      vertex.normal.y = mesh.vertex_data[normal_base + 1];
      vertex.normal.z = mesh.vertex_data[normal_base + 2];
    }
    const std::size_t uv_base =
        static_cast<std::size_t>(i) * floats_per_vertex + uv_offset / sizeof(float);
    if (uv_offset != 0 && uv_base + 1 < max_float_index) {
      vertex.uv[0] = mesh.vertex_data[uv_base];
      vertex.uv[1] = mesh.vertex_data[uv_base + 1];
    }
    extracted.vertices[i] = vertex;
  }

  // BF2 stores triangle indices relative to each material's vertex_start,
  // so the true global vertex index is vertex_start + stored_index.
  for (const auto& material : lod.materials) {
    if (material.index_start + material.index_count > mesh.indices.size()) {
      continue;
    }
    for (std::uint32_t i = 0; i < material.index_count; ++i) {
      const std::uint32_t index = material.vertex_start + mesh.indices[material.index_start + i];
      if (index < max_vertex) {
        extracted.indices.push_back(index);
      }
    }
  }

    return extracted;
}

TexturedMeshData MeshLoader::extract_textured(const Mesh& mesh, std::size_t geometry_index,
                                              std::size_t lod_index) {
  TexturedMeshData out;
  if (geometry_index >= mesh.geometries.size()) {
    return out;
  }
  const auto& geometry = mesh.geometries[geometry_index];
  if (lod_index >= geometry.lods.size()) {
    return out;
  }
  const auto& lod = geometry.lods[lod_index];

  std::uint16_t position_offset = 0;
  std::uint16_t normal_offset = 0;
  std::uint16_t tangent_offset = 0;
  std::uint16_t uv0_offset = 0;
  std::uint16_t uv1_offset = 0;
  std::uint16_t lm_offset = 0;  // lightmap UV = highest texcoord channel
  bool have_tangent = false;
  bool have_uv0 = false;
  bool have_uv1 = false;
  bool have_lm = false;
  int max_uv_channel = -1;
  for (const auto& attr : mesh.vertex_attributes) {
    if (attr.flag != 0) {
      continue;
    }
    if (attr.usage == 0) {
      position_offset = attr.offset;
    } else if (attr.usage == 3) {
      normal_offset = attr.offset;
    } else if (attr.usage == 6) {  // TANGENT
      tangent_offset = attr.offset;
      have_tangent = true;
    } else if ((attr.usage & 0xFF) == 5) {  // TEXCOORD; channel index in high byte
      const int channel = attr.usage >> 8;
      if (channel == 0) {
        uv0_offset = attr.offset;
        have_uv0 = true;
      } else if (channel == 1) {
        uv1_offset = attr.offset;
        have_uv1 = true;
      }
      // BF2 staticmesh lightmap UV is the last texcoord set (typically channel
      // 4); only treat channels >= 2 as a lightmap UV so we never mistake the
      // detail UV (channel 1) for it.
      if (channel >= 2 && channel > max_uv_channel) {
        max_uv_channel = channel;
        lm_offset = attr.offset;
        have_lm = true;
      }
    }
  }

  const auto floats_per_vertex =
      mesh.vertex_stride / std::max<std::uint32_t>(mesh.vertex_format, 1u);
  const auto max_float_index = mesh.vertex_data.size();

  std::uint32_t max_vertex = mesh.vertex_count;
  for (const auto& material : lod.materials) {
    max_vertex = std::max(max_vertex, material.vertex_start + material.vertex_count);
  }
  max_vertex = std::min(max_vertex, mesh.vertex_count);
  out.vertices.resize(max_vertex);

  auto read2 = [&](std::uint32_t v, std::uint16_t off, float* dst) {
    const std::size_t b = static_cast<std::size_t>(v) * floats_per_vertex + off / sizeof(float);
    if (b + 1 < max_float_index) {
      dst[0] = mesh.vertex_data[b];
      dst[1] = mesh.vertex_data[b + 1];
    }
  };
  auto read3 = [&](std::uint32_t v, std::uint16_t off, Float3& dst) {
    const std::size_t b = static_cast<std::size_t>(v) * floats_per_vertex + off / sizeof(float);
    if (b + 2 < max_float_index) {
      dst.x = mesh.vertex_data[b];
      dst.y = mesh.vertex_data[b + 1];
      dst.z = mesh.vertex_data[b + 2];
    }
  };

  for (std::uint32_t i = 0; i < max_vertex; ++i) {
    ExtractedVertex v;
    read3(i, position_offset, v.position);
    read3(i, normal_offset, v.normal);
    if (have_tangent) read3(i, tangent_offset, v.tangent);
    if (have_uv0) read2(i, uv0_offset, v.uv);
    if (have_uv1) read2(i, uv1_offset, v.uv1);
    if (have_lm) read2(i, lm_offset, v.uv_lm);
    out.vertices[i] = v;
  }

  auto is_detail = [](const std::string& p) {
    std::string l = p;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
    return l.find("_de") != std::string::npos || l.find("_detail") != std::string::npos;
  };

  for (const auto& material : lod.materials) {
    if (material.index_start + material.index_count > mesh.indices.size()) {
      continue;
    }
    TexturedSubmesh sub;
    sub.index_offset = static_cast<std::uint32_t>(out.indices.size());
    for (std::uint32_t i = 0; i < material.index_count; ++i) {
      const std::uint32_t index = material.vertex_start + mesh.indices[material.index_start + i];
      if (index < max_vertex) {
        out.indices.push_back(index);
      }
    }
    sub.index_count = static_cast<std::uint32_t>(out.indices.size()) - sub.index_offset;
    // Classify the material's texture layers by BF2 suffix so we don't depend on
    // slot order (which varies by technique): _c base, _de detail, _deb normal,
    // _di dirt, _cr crack. SpecularLUT and anything else is ignored.
    if (!material.maps.empty()) {
      sub.base_map = material.maps[0];
    }
    auto stem_lower = [](std::string p) {
      const auto dot = p.find_last_of('.');
      if (dot != std::string::npos) p = p.substr(0, dot);
      std::transform(p.begin(), p.end(), p.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      return p;
    };
    auto ends_with = [](const std::string& s, const char* suf) {
      const std::size_t n = std::strlen(suf);
      return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    for (const auto& m : material.maps) {
      const std::string s = stem_lower(m);
      if (ends_with(s, "_deb") || ends_with(s, "_b")) {
        sub.normal_map = m;
      } else if (ends_with(s, "_de")) {
        sub.detail_map = m;
      } else if (ends_with(s, "_di")) {
        sub.dirt_map = m;
      } else if (ends_with(s, "_cr")) {
        sub.crack_map = m;
      } else if (ends_with(s, "_c")) {
        sub.base_map = m;
      }
    }
    (void)is_detail;
    out.submeshes.push_back(std::move(sub));
  }

  return out;
}

}  // namespace bf2
