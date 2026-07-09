#pragma once

#include <cstdint>

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

// CPU-skin a SkinnedMesh LOD. Each vertex is deformed by up to two rig bones
// (BF2 uses a single blend weight w -> bone0=w, bone1=1-w). The mesh rig
// matrices are the authoritative bind pose; the animation clip (optional; pass
// nullptr for bind pose) is applied as per-bone local deltas retargeted onto
// that bind. Returns deformed geometry ready to upload/render.
ExtractedMesh skin_mesh(const Mesh& mesh, const Skeleton& skeleton, const AnimationClip* clip,
                        int frame, std::size_t geometry_index = 0, std::size_t lod_index = 0);

// Static skinned geometry for GPU skinning. Positions/normals are the authored
// (bind-pose, model-space) attributes; bone[] holds the two skeleton node
// indices each vertex is bound to and weight is the blend for bone[0]. Upload
// this buffer once; drive deformation on the GPU with compute_skin_palette().
struct SkinnedVertex {
    Float3 position{};
    Float3 normal{};
    float uv[2]{};
    std::int32_t bone[2]{};
    float weight = 1.0f;
};

// A contiguous run of indices sharing one material/texture (e.g. a soldier's
// body vs. head use different colour maps). `diffuse_map` is the texture path to
// resolve; the caller binds the GL id per submesh at draw time.
struct SkinnedSubmesh {
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    std::string diffuse_map;
};

struct SkinnedGeometry {
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SkinnedSubmesh> submeshes;
    int node_count = 0;
};

SkinnedGeometry extract_skinned(const Mesh& mesh, const Skeleton& skeleton,
                                std::size_t geometry_index = 0, std::size_t lod_index = 0);

// Per-frame skinning palette indexed by skeleton node. Multiply an authored
// vertex position by palette[bone] to obtain its animated model-space position
// (identical math to skin_mesh, but suitable for a GPU bone-matrix uniform).
// Pass clip == nullptr for the bind pose.
std::vector<glm::mat4> compute_skin_palette(const Mesh& mesh, const Skeleton& skeleton,
                                            const AnimationClip* clip, int frame,
                                            std::size_t geometry_index = 0,
                                            std::size_t lod_index = 0);

}  // namespace bf2
