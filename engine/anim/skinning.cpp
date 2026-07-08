#include "skinning.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/anim/pose.hpp"

namespace bf2 {
namespace {

// Load a BF2 (row-major, row-vector) matrix directly into a glm mat4. Because
// glm is column-major, a direct element copy yields the column-vector operator
// equivalent to BF2's row-vector matrix.
glm::mat4 to_glm(const Matrix4& m) {
  glm::mat4 g(1.0f);
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      g[i][j] = m.m[i][j];
    }
  }
  return g;
}

// Normalize a quaternion, falling back to identity if it is degenerate
// (zero-length / NaN / inf). Prevents a single bad joint from producing NaNs
// that propagate down the hierarchy as runaway "sail" triangles.
glm::quat safe_normalize(const glm::quat& q) {
  const float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (!std::isfinite(len2) || len2 < 1e-12f) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  return q * (1.0f / std::sqrt(len2));
}

// Extract a clean rotation from a transform that may carry scale/shear. Some
// BF2 skinned-mesh bind matrices bake non-uniform scale into the basis, which
// makes glm::quat_cast return a garbage/NaN quaternion and flings every vertex
// weighted to that bone off to infinity. We Gram-Schmidt orthonormalize the
// upper 3x3 into a proper (right-handed) rotation first, then guard the result.
glm::quat safe_quat(const glm::mat4& m) {
  glm::vec3 x(m[0]);
  glm::vec3 y(m[1]);
  const float lx = glm::length(x);
  x = lx > 1e-8f ? x / lx : glm::vec3(1.0f, 0.0f, 0.0f);
  y = y - x * glm::dot(x, y);
  const float ly = glm::length(y);
  y = ly > 1e-8f ? y / ly : glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::vec3 z = glm::cross(x, y);  // proper right-handed basis
  const glm::mat3 r(x, y, z);
  return safe_normalize(glm::quat_cast(r));
}

struct Offsets {
  int position = -1;
  int normal = -1;
  int uv = -1;
  int blend_weight = -1;
  int blend_indices = -1;
};

Offsets find_offsets(const Mesh& mesh) {
  Offsets o;
  for (const auto& attr : mesh.vertex_attributes) {
    if (attr.flag != 0) {
      continue;
    }
    switch (attr.usage) {
      case 0: o.position = attr.offset; break;
      case 1: o.blend_weight = attr.offset; break;
      case 2: o.blend_indices = attr.offset; break;
      case 3: o.normal = attr.offset; break;
      case 5:
      case 0x101:
      case 0x105:
        if (o.uv < 0) o.uv = attr.offset;
        break;
      default: break;
    }
  }
  return o;
}

// Compute the per-node skinning palette: palette[node] * bind_pos = animated
// model-space position. This is the single source of truth shared by CPU
// skinning (skin_mesh) and GPU skinning (compute_skin_palette).
std::vector<glm::mat4> build_palette(const Skeleton& skeleton, const Lod& lod,
                                     const AnimationClip* clip, int frame) {
  const std::size_t node_count = skeleton.nodes.size();
  const bool debug = std::getenv("BF2_SKIN_DEBUG") != nullptr;

  // --- Authoritative mesh bind (bone-local -> model) per skeleton node. ---
  const PosedSkeleton ske_bind = pose_skeleton(skeleton, nullptr, 0);
  const PosedSkeleton ske_anim = pose_skeleton(skeleton, clip, frame);
  std::vector<glm::mat4> mesh_bind(node_count, glm::mat4(1.0f));
  std::vector<bool> has_bind(node_count, false);
  for (const auto& rig : lod.rigs) {
    for (const auto& bone : rig.bones) {
      if (bone.id < node_count && !has_bind[bone.id]) {
        mesh_bind[bone.id] = to_glm(bone.transform);
        has_bind[bone.id] = true;
      }
    }
  }
  for (std::size_t i = 0; i < node_count; ++i) {
    if (has_bind[i]) continue;
    int anchor = static_cast<int>(i);
    while (anchor >= 0 && !has_bind[anchor]) anchor = skeleton.nodes[anchor].parent;
    if (anchor >= 0) {
      mesh_bind[i] = mesh_bind[anchor] *
                     glm::inverse(ske_bind.world_transforms[anchor]) * ske_bind.world_transforms[i];
    } else {
      mesh_bind[i] = ske_bind.world_transforms[i];
    }
  }

  std::vector<glm::mat4> skin(node_count, glm::mat4(1.0f));

  float bind_mismatch = 0.f;
  for (std::size_t i = 0; i < node_count; ++i) {
    if (!has_bind[i]) continue;
    const glm::mat4 d = ske_bind.world_transforms[i] * glm::inverse(mesh_bind[i]);
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) bind_mismatch += d[c][r] * d[c][r];
  }

  const bool is_character = node_count >= 50;

  if (!clip) {
    // Exact authored bind pose via mesh hierarchy.
    std::vector<glm::mat4> world(node_count, glm::mat4(1.0f));
    for (std::size_t i = 0; i < node_count; ++i) {
      const int p = skeleton.nodes[i].parent;
      if (p >= 0 && p < static_cast<int>(i)) {
        const glm::mat4 bind_local = glm::inverse(mesh_bind[p]) * mesh_bind[i];
        const glm::vec3 offset = glm::vec3(bind_local[3]);
        const glm::quat mesh_local_q = safe_quat(bind_local);
        world[i] = world[p] * glm::translate(glm::mat4(1.0f), offset) * glm::mat4_cast(mesh_local_q);
      } else {
        const glm::vec3 t = glm::vec3(mesh_bind[i][3]);
        const glm::quat mesh_root_q = safe_quat(mesh_bind[i]);
        world[i] = glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(mesh_root_q);
      }
    }
    for (std::size_t i = 0; i < node_count; ++i) {
      skin[i] = world[i] * glm::inverse(mesh_bind[i]);
    }
  } else if (is_character) {
    // The authored mesh bind differs from the skeleton rest pose (bind_mismatch
    // is large), so we build the animated hierarchy on the *mesh* bind (exactly
    // like the bind-pose branch) and layer on only the clip's per-bone *local
    // rotation delta* (rest-local^-1 * anim-local). With a still clip the delta
    // is identity, reproducing the correct bind; motion rotates joints in place
    // rather than splaying limbs out. Translation deltas are ignored so the feet
    // stay planted under the body we position in world space.
    auto ske_local_rot = [&](const PosedSkeleton& p, std::size_t i, int parent) {
      glm::mat4 local = (parent >= 0 && parent < static_cast<int>(i))
                            ? glm::inverse(p.world_transforms[parent]) * p.world_transforms[i]
                            : p.world_transforms[i];
      return safe_quat(local);
    };
    std::vector<glm::mat4> world(node_count, glm::mat4(1.0f));
    for (std::size_t i = 0; i < node_count; ++i) {
      const int p = skeleton.nodes[i].parent;
      const glm::quat qb = ske_local_rot(ske_bind, i, p);
      const glm::quat qa = ske_local_rot(ske_anim, i, p);
      const glm::quat delta = safe_normalize(glm::inverse(qb) * qa);
      if (p >= 0 && p < static_cast<int>(i)) {
        const glm::mat4 bind_local = glm::inverse(mesh_bind[p]) * mesh_bind[i];
        const glm::vec3 offset = glm::vec3(bind_local[3]);
        const glm::quat qmesh = safe_quat(bind_local);
        world[i] =
            world[p] * glm::translate(glm::mat4(1.0f), offset) * glm::mat4_cast(safe_normalize(qmesh * delta));
      } else {
        const glm::vec3 t = glm::vec3(mesh_bind[i][3]);
        const glm::quat qmesh = safe_quat(mesh_bind[i]);
        world[i] = glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(safe_normalize(qmesh * delta));
      }
    }
    for (std::size_t i = 0; i < node_count; ++i) {
      skin[i] = world[i] * glm::inverse(mesh_bind[i]);
    }
  } else {
    auto world_rot = [&](std::size_t i) {
      return safe_quat(ske_anim.world_transforms[i]);
    };
    std::vector<glm::mat4> world(node_count, glm::mat4(1.0f));
    for (std::size_t i = 0; i < node_count; ++i) {
      const int p = skeleton.nodes[i].parent;
      if (p >= 0 && p < static_cast<int>(i)) {
        const glm::mat4 bind_local = glm::inverse(mesh_bind[p]) * mesh_bind[i];
        const glm::vec3 offset = glm::vec3(bind_local[3]);
        const glm::quat anim_local = safe_normalize(glm::inverse(world_rot(p)) * world_rot(i));
        world[i] = world[p] * glm::translate(glm::mat4(1.0f), offset) * glm::mat4_cast(anim_local);
      } else {
        const glm::vec3 t = glm::vec3(mesh_bind[i][3]);
        world[i] = glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(world_rot(i));
      }
    }
    for (std::size_t i = 0; i < node_count; ++i) {
      skin[i] = world[i] * glm::inverse(mesh_bind[i]);
    }
  }

  if (debug) {
    std::printf("  bind_mismatch=%.3f clip=%s nodes=%zu\n", bind_mismatch, clip ? "yes" : "no",
                node_count);
  }
  return skin;
}

const Lod* select_lod(const Mesh& mesh, std::size_t geometry_index, std::size_t lod_index) {
  if (geometry_index >= mesh.geometries.size()) return nullptr;
  const auto& geometry = mesh.geometries[geometry_index];
  if (lod_index >= geometry.lods.size()) return nullptr;
  return &geometry.lods[lod_index];
}

}  // namespace

ExtractedMesh skin_mesh(const Mesh& mesh, const Skeleton& skeleton, const AnimationClip* clip,
                        int frame, std::size_t geometry_index, std::size_t lod_index) {
  ExtractedMesh out;
  const Lod* lod_ptr = select_lod(mesh, geometry_index, lod_index);
  if (!lod_ptr) {
    return out;
  }
  const auto& lod = *lod_ptr;
  const std::size_t node_count = skeleton.nodes.size();
  const std::vector<glm::mat4> skin = build_palette(skeleton, lod, clip, frame);

  // --- Deform vertices. ---
  const Offsets off = find_offsets(mesh);
  const auto* raw = reinterpret_cast<const std::uint8_t*>(mesh.vertex_data.data());
  const std::size_t raw_size = mesh.vertex_data.size() * sizeof(float);
  const std::uint32_t stride = mesh.vertex_stride;

  auto read_f = [&](std::size_t byte) -> float {
    float v = 0.f;
    if (byte + sizeof(float) <= raw_size) std::memcpy(&v, raw + byte, sizeof(float));
    return v;
  };
  auto read_vec3 = [&](std::size_t byte) {
    return glm::vec3(read_f(byte), read_f(byte + 4), read_f(byte + 8));
  };

  out.materials = lod.materials;
  out.vertices.resize(mesh.vertex_count);
  for (std::uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const std::size_t base = static_cast<std::size_t>(i) * stride;
    ExtractedVertex v;
    if (off.position >= 0) {
      const glm::vec3 p = read_vec3(base + off.position);
      v.position = {p.x, p.y, p.z};
    }
    if (off.uv >= 0) {
      v.uv[0] = read_f(base + off.uv);
      v.uv[1] = read_f(base + off.uv + 4);
    }
    out.vertices[i] = v;
  }

  for (std::size_t mi = 0; mi < lod.materials.size(); ++mi) {
    const auto& material = lod.materials[mi];
    if (mi >= lod.rigs.size() || lod.rigs[mi].bones.empty()) continue;
    const auto& rig = lod.rigs[mi];

    // Skin matrix indexed by rig-local bone slot.
    std::vector<glm::mat4> slot(rig.bones.size(), glm::mat4(1.0f));
    for (std::size_t b = 0; b < rig.bones.size(); ++b) {
      const std::uint32_t node = rig.bones[b].id;
      slot[b] = node < node_count ? skin[node] : glm::mat4(1.0f);
    }

    const std::uint32_t v0 = material.vertex_start;
    const std::uint32_t v1 =
        std::min(material.vertex_start + material.vertex_count, mesh.vertex_count);
    for (std::uint32_t i = v0; i < v1; ++i) {
      const std::size_t base = static_cast<std::size_t>(i) * stride;
      const glm::vec3 pos = off.position >= 0 ? read_vec3(base + off.position) : glm::vec3(0);
      const glm::vec3 nrm = off.normal >= 0 ? read_vec3(base + off.normal) : glm::vec3(0, 1, 0);

      float w0 = 1.0f;
      int b0 = 0, b1 = 0;
      if (off.blend_weight >= 0) w0 = read_f(base + off.blend_weight);
      if (off.blend_indices >= 0 && base + off.blend_indices + 1 < raw_size) {
        b0 = raw[base + off.blend_indices];
        b1 = raw[base + off.blend_indices + 1];
      }
      b0 = std::clamp(b0, 0, static_cast<int>(rig.bones.size()) - 1);
      b1 = std::clamp(b1, 0, static_cast<int>(rig.bones.size()) - 1);
      if (!std::isfinite(w0)) w0 = 1.0f;
      w0 = std::clamp(w0, 0.0f, 1.0f);
      if (b0 == b1 || w0 > 0.98f) w0 = 1.0f;
      if (w0 < 0.02f) {
        w0 = 0.0f;
        b0 = b1;
      }
      const float w1 = 1.0f - w0;

      const glm::mat4 m = slot[b0] * w0 + slot[b1] * w1;
      const glm::vec3 sp = glm::vec3(m * glm::vec4(pos, 1.0f));
      const glm::vec3 sn = glm::normalize(glm::mat3(m) * nrm);
      out.vertices[i].position = {sp.x, sp.y, sp.z};
      out.vertices[i].normal = {sn.x, sn.y, sn.z};
    }
  }

  for (const auto& material : lod.materials) {
    if (material.index_start + material.index_count > mesh.indices.size()) continue;
    for (std::uint32_t i = 0; i < material.index_count; ++i) {
      const std::uint32_t index = material.vertex_start + mesh.indices[material.index_start + i];
      if (index < out.vertices.size()) out.indices.push_back(index);
    }
  }

  return out;
}

SkinnedGeometry extract_skinned(const Mesh& mesh, const Skeleton& skeleton,
                                std::size_t geometry_index, std::size_t lod_index) {
  SkinnedGeometry out;
  const Lod* lod_ptr = select_lod(mesh, geometry_index, lod_index);
  if (!lod_ptr) {
    return out;
  }
  const auto& lod = *lod_ptr;
  out.node_count = static_cast<int>(skeleton.nodes.size());

  const Offsets off = find_offsets(mesh);
  const auto* raw = reinterpret_cast<const std::uint8_t*>(mesh.vertex_data.data());
  const std::size_t raw_size = mesh.vertex_data.size() * sizeof(float);
  const std::uint32_t stride = mesh.vertex_stride;

  auto read_f = [&](std::size_t byte) -> float {
    float v = 0.f;
    if (byte + sizeof(float) <= raw_size) std::memcpy(&v, raw + byte, sizeof(float));
    return v;
  };

  out.vertices.resize(mesh.vertex_count);
  for (std::uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const std::size_t base = static_cast<std::size_t>(i) * stride;
    SkinnedVertex v;
    if (off.position >= 0) {
      v.position = {read_f(base + off.position), read_f(base + off.position + 4),
                    read_f(base + off.position + 8)};
    }
    if (off.normal >= 0) {
      v.normal = {read_f(base + off.normal), read_f(base + off.normal + 4),
                  read_f(base + off.normal + 8)};
    }
    if (off.uv >= 0) {
      v.uv[0] = read_f(base + off.uv);
      v.uv[1] = read_f(base + off.uv + 4);
    }
    out.vertices[i] = v;
  }

  // Map each material's rig-local blend indices to global skeleton node ids so
  // the GPU can index one shared bone palette.
  for (std::size_t mi = 0; mi < lod.materials.size(); ++mi) {
    const auto& material = lod.materials[mi];
    const std::uint32_t v0 = material.vertex_start;
    const std::uint32_t v1 =
        std::min(material.vertex_start + material.vertex_count, mesh.vertex_count);
    const bool has_rig = mi < lod.rigs.size() && !lod.rigs[mi].bones.empty();
    const auto* rig = has_rig ? &lod.rigs[mi] : nullptr;

    for (std::uint32_t i = v0; i < v1; ++i) {
      const std::size_t base = static_cast<std::size_t>(i) * stride;
      float w0 = 1.0f;
      int b0 = 0, b1 = 0;
      if (off.blend_weight >= 0) w0 = read_f(base + off.blend_weight);
      if (off.blend_indices >= 0 && base + off.blend_indices + 1 < raw_size) {
        b0 = raw[base + off.blend_indices];
        b1 = raw[base + off.blend_indices + 1];
      }
      int node0 = 0, node1 = 0;
      if (rig) {
        b0 = std::clamp(b0, 0, static_cast<int>(rig->bones.size()) - 1);
        b1 = std::clamp(b1, 0, static_cast<int>(rig->bones.size()) - 1);
        node0 = static_cast<int>(rig->bones[b0].id);
        node1 = static_cast<int>(rig->bones[b1].id);
      }
      // Sanitize the weight: keep it in [0,1] and snap near-rigid vertices to a
      // single bone so a stray/garbage secondary index can never drag a vertex.
      if (!std::isfinite(w0)) w0 = 1.0f;
      w0 = std::clamp(w0, 0.0f, 1.0f);
      if (b0 == b1) w0 = 1.0f;
      if (w0 > 0.98f) w0 = 1.0f;
      if (w0 < 0.02f) {
        w0 = 0.0f;
        node0 = node1;  // fully bound to the second bone
      }
      out.vertices[i].bone[0] = node0;
      out.vertices[i].bone[1] = node1;
      out.vertices[i].weight = w0;
    }
  }

  for (const auto& material : lod.materials) {
    if (material.index_start + material.index_count > mesh.indices.size()) continue;
    for (std::uint32_t i = 0; i < material.index_count; ++i) {
      const std::uint32_t index = material.vertex_start + mesh.indices[material.index_start + i];
      if (index < out.vertices.size()) out.indices.push_back(index);
    }
  }
  return out;
}

std::vector<glm::mat4> compute_skin_palette(const Mesh& mesh, const Skeleton& skeleton,
                                            const AnimationClip* clip, int frame,
                                            std::size_t geometry_index, std::size_t lod_index) {
  const Lod* lod_ptr = select_lod(mesh, geometry_index, lod_index);
  if (!lod_ptr) {
    return {};
  }
  return build_palette(skeleton, *lod_ptr, clip, frame);
}

}  // namespace bf2
