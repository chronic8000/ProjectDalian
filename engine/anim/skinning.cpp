#include "skinning.hpp"

#include <algorithm>
#include <cctype>
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
[[maybe_unused]] glm::quat safe_quat(const glm::mat4& m) {
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
  PosedSkeleton ske_anim = pose_skeleton(skeleton, clip, frame);
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

  // BF2 locomotion clips only animate the legs, spine and root (~16 bones); the
  // arms come from a separate per-weapon overlay we don't load, so left untouched
  // they render in the skeleton's A-pose (arms splayed out sideways). Layer a
  // static rifle-hold onto the arm subtrees: rotate the shoulders down/forward and
  // bend the elbows in, so the soldier grips his weapon. Applied only when a clip
  // is playing and the rig looks like a character (has the named arm bones).
  if (clip && node_count >= 48) {
    // Apply an extra rotation in a bone's local frame to that bone and every
    // descendant (nodes are stored parent-before-child).
    auto apply_hold = [&](int bone, float ax, float ay, float az) {
      if (bone < 0 || bone >= static_cast<int>(node_count)) return;
      const glm::quat r = glm::quat(glm::vec3(glm::radians(ax), glm::radians(ay), glm::radians(az)));
      const glm::mat4 w = ske_anim.world_transforms[bone];
      const glm::mat4 M = w * glm::mat4_cast(r) * glm::inverse(w);
      for (int d = bone; d < static_cast<int>(node_count); ++d) {
        int p = d;
        bool desc = false;
        while (p >= 0) {
          if (p == bone) { desc = true; break; }
          p = skeleton.nodes[p].parent;
        }
        if (desc) ske_anim.world_transforms[d] = M * ske_anim.world_transforms[d];
      }
    };
    auto ang = [](const char* env, float d0, float d1, float d2, float* out) {
      out[0] = d0; out[1] = d1; out[2] = d2;
      if (const char* s = std::getenv(env)) std::sscanf(s, "%f %f %f", &out[0], &out[1], &out[2]);
    };
    float s[3], e[3];
    ang("BF2_ARM_S", -66.f, 8.f, 20.f, s);  // shoulder: arm down + forward + inward
    ang("BF2_ARM_E", 0.f, 78.f, 8.f, e);    // elbow: bend forearm across the chest
    // Left arm, then right arm mirrored (flip the Y/Z components).
    apply_hold(15, s[0], s[1], s[2]);
    apply_hold(16, e[0], e[1], e[2]);
    apply_hold(31, s[0], -s[1], -s[2]);
    apply_hold(32, e[0], -e[1], -e[2]);
  }

  // Linear blend skinning. In the BF2 skinnedmesh format each rig bone matrix is
  // the *inverse bind* (model-space -> bone-local at the authored bind pose), so
  // the deform matrix is simply the bone's animated world transform composed with
  // that inverse bind. Bones the rig doesn't list fall back to the skeleton's own
  // rest pose. With a null/still clip the animated world equals the bind, so this
  // reproduces the exact bind; motion then rotates each joint about its true
  // parent, so arms and legs articulate correctly instead of splaying out.
  for (std::size_t i = 0; i < node_count; ++i) {
    const glm::mat4 inv_bind =
        has_bind[i] ? mesh_bind[i] : glm::inverse(ske_bind.world_transforms[i]);
    skin[i] = ske_anim.world_transforms[i] * inv_bind;
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

  // Pick the colour map from a material's map list: the first map that isn't a
  // bump/normal/specular. Soldiers carry separate body and head colour maps, so
  // each material becomes its own textured submesh.
  auto pick_diffuse_map = [](const Material& m) -> std::string {
    std::string first;
    for (const auto& mp : m.maps) {
      if (mp.empty()) continue;
      if (first.empty()) first = mp;
      std::string s = mp;
      for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
      if (s.find("_b.") == std::string::npos && s.find("bump") == std::string::npos &&
          s.find("normal") == std::string::npos && s.find("_s.") == std::string::npos &&
          s.find("spec") == std::string::npos) {
        return mp;
      }
    }
    return first;
  };

  for (const auto& material : lod.materials) {
    if (material.index_start + material.index_count > mesh.indices.size()) continue;
    SkinnedSubmesh sub;
    sub.index_offset = static_cast<std::uint32_t>(out.indices.size());
    sub.diffuse_map = pick_diffuse_map(material);
    for (std::uint32_t i = 0; i < material.index_count; ++i) {
      const std::uint32_t index = material.vertex_start + mesh.indices[material.index_start + i];
      if (index < out.vertices.size()) out.indices.push_back(index);
    }
    sub.index_count = static_cast<std::uint32_t>(out.indices.size()) - sub.index_offset;
    if (sub.index_count > 0) out.submeshes.push_back(sub);
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
