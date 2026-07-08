#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/anim/skinning.hpp"
#include "engine/formats/dds/dds_loader.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

struct GpuMesh {
  std::uint32_t vao = 0;
  std::uint32_t vbo = 0;
  std::uint32_t ebo = 0;
  std::uint32_t index_count = 0;
};

// A drawable range of a textured mesh sharing base/detail textures.
struct GpuSubmesh {
  std::uint32_t index_offset = 0;
  std::uint32_t index_count = 0;
  std::uint32_t base_tex = 0;
  std::uint32_t detail_tex = 0;
  std::uint32_t normal_tex = 0;
  std::uint32_t dirt_tex = 0;
  std::uint32_t crack_tex = 0;
};

// A textured mesh with vertices pos(3)/normal(3)/uv0(2)/uv1(2) already in world
// space; each submesh binds its own textures. Draw with a plain view-projection.
struct GpuTexturedMesh {
  std::uint32_t vao = 0;
  std::uint32_t vbo = 0;
  std::uint32_t ebo = 0;
  std::vector<GpuSubmesh> submeshes;
};

// A flat/lit colored mesh (position + normal), used for procedural geometry such
// as the first-person weapon viewmodel and simple effect shapes.
struct GpuColorMesh {
  std::uint32_t vao = 0;
  std::uint32_t vbo = 0;
  std::uint32_t ebo = 0;
  std::uint32_t index_count = 0;
};

// A skinned mesh whose bind-pose geometry lives on the GPU. Deformation happens
// entirely in the vertex shader from a per-frame bone-matrix palette, so the
// vertex buffer is uploaded once and never re-streamed.
struct GpuSkinnedMesh {
  std::uint32_t vao = 0;
  std::uint32_t vbo = 0;
  std::uint32_t ebo = 0;
  std::uint32_t index_count = 0;
};

// Maximum bones addressable by the skinning shader's uniform palette.
inline constexpr int kMaxSkinBones = 128;

class Renderer {
public:
  bool initialize(void* sdl_window);
  void shutdown();
  void begin_frame(float r, float g, float b);
  void end_frame();
  void set_viewport(int width, int height);
  GpuMesh upload_mesh(const ExtractedMesh& mesh);
  void draw_mesh(const GpuMesh& mesh, const float* view_projection);
  void destroy_mesh(GpuMesh& mesh);

  GpuSkinnedMesh upload_skinned(const SkinnedGeometry& geometry);
  // palette: array of node_count mat4 (column-major), one per skeleton node.
  // diffuse_tex: soldier skin texture (0 = flat). model: world transform for
  // lighting/shadows/fog. tint3: per-instance colour multiply (nullptr = white,
  // used to redden hit flashes or distinguish factions).
  void draw_skinned(const GpuSkinnedMesh& mesh, const float* view_projection, const float* palette,
                    int bone_count, std::uint32_t diffuse_tex = 0, const float* model = nullptr,
                    const float* tint3 = nullptr);
  void destroy_skinned(GpuSkinnedMesh& mesh);

  // Textures & textured meshes.
  std::uint32_t upload_texture(const DdsTexture& texture);
  void destroy_texture(std::uint32_t texture);
  // Uploads world-space textured geometry; the returned mesh's submeshes come
  // from data.submeshes with texture ids filled in by the caller.
  GpuTexturedMesh upload_textured(const TexturedMeshData& data);
  // mvp = viewProj * model (column-major). model is used to rotate normals for
  // lighting; pass nullptr when vertices are already in world space. When
  // obj_lightmap != 0 the mesh's lightmap UV is sampled from that atlas texture,
  // transformed by lm_xform (xy = uv scale, zw = uv offset) -- BF2's per-instance
  // baked object lighting.
  void draw_textured(const GpuTexturedMesh& mesh, const float* mvp, const float* model = nullptr,
                     std::uint32_t obj_lightmap = 0, const float* lm_xform = nullptr);
  void destroy_textured(GpuTexturedMesh& mesh);

  // Terrain draw inputs: colormap (macro colour) + optional lightmap, plus an
  // optional per-patch detail splat (base/layer1/layer2 tiling details blended
  // by two green-channel masks).
  struct TerrainDraw {
    std::uint32_t colormap = 0;
    std::uint32_t lightmap = 0;
    std::uint32_t mask1 = 0;
    std::uint32_t mask2 = 0;
    std::uint32_t detail0 = 0;
    std::uint32_t detail1 = 0;
    std::uint32_t detail2 = 0;
    float detail_tiling = 64.f;
  };
  void draw_terrain_colormap(const GpuTexturedMesh& mesh, const float* mvp, const TerrainDraw& t);

  // Colored geometry. pos_normal is interleaved position(3)+normal(3) per vertex.
  GpuColorMesh upload_color(const std::vector<float>& pos_normal,
                            const std::vector<std::uint32_t>& indices);
  // mvp = viewProj*model; model rotates normals. lit toggles diffuse shading;
  // depth_test=false draws the mesh over everything (viewmodel/effects).
  void draw_color(const GpuColorMesh& mesh, const float* mvp, const float* model, float r, float g,
                  float b, bool lit, bool depth_test);
  void destroy_color(GpuColorMesh& mesh);

  // Immediate colored lines. xyz holds vertex_count positions (3 floats each),
  // consumed as GL_LINES pairs. Unlit, constant color.
  void draw_lines(const float* mvp, const float* xyz, int vertex_count, float r, float g, float b,
                  float width, bool depth_test);

  // Per-frame environment applied to terrain/object/water shaders: camera world
  // position (for fog distance), sun direction (light travel direction), fog
  // colour, and fog start/end distances (fog_end <= 0 disables fog).
  void set_environment(const float* cam_pos3, const float* sun_dir3, const float* fog_color3,
                       float fog_start, float fog_end);

  // ---- Cascaded Shadow Maps -------------------------------------------------
  static constexpr int kShadowCascades = 4;
  // Begin a depth-only pass for one cascade. `light_view_proj` is that cascade's
  // world->light-clip matrix (column-major). Draw occluders with draw_depth().
  void begin_shadow_pass(int cascade, const float* light_view_proj);
  // Render a textured mesh's geometry (position only) into the bound shadow map.
  void draw_depth(const GpuTexturedMesh& mesh, const float* model);
  // Publish the cascade matrices/splits so lit shaders sample the shadow maps.
  // splits = per-cascade far distance (world units). enabled toggles sampling.
  void set_shadows(const float* cascade_view_proj_4x16, const float* splits4, bool enabled);
  int shadow_resolution() const { return shadow_res_; }

  // ---- Offscreen scene capture (for post-processing) ------------------------
  // Bind an offscreen colour+depth target sized (w,h) and clear it. All world
  // draws after this land in the offscreen buffer instead of the backbuffer.
  void begin_scene(int w, int h, float r, float g, float b);
  // Resolve the offscreen scene to the default framebuffer through the FPV
  // post-process shader. `degrade` (0..1) scales the video-feed artefacts.
  void present_scene(float degrade, float time_seconds);

  // Full-screen gradient sky. inv_view_proj reconstructs world ray directions;
  // the gradient runs from horizon_color (at the horizon) to sky_color (zenith).
  void draw_sky(const float* inv_view_proj, const float* cam_pos3, const float* sky_color3,
                const float* horizon_color3);

  // Translucent animated water plane at world height level_y, centred on the
  // camera's XZ and extending half_extent metres in every direction.
  void draw_water(const float* view_proj, float level_y, float cam_x, float cam_z, float half_extent,
                  float time_seconds, const float* water_color3);

  // Alpha-tested grass billboards. verts is interleaved [pos.xyz, uv.xy, sway]
  // (6 floats/vertex) in world space; sway (0 at blade base, 1 at tip) drives the
  // wind animation. Uploaded to an internal dynamic buffer each call.
  void draw_grass(const float* view_proj, const float* cam_pos3, std::uint32_t atlas_tex,
                  const float* verts, int vertex_count, float time_seconds);

private:
  void apply_environment(std::uint32_t program) const;
  void apply_shadows(std::uint32_t program, int sampler_unit) const;

  std::uint32_t shader_program_ = 0;
  std::uint32_t skin_program_ = 0;
  std::uint32_t textured_program_ = 0;
  std::uint32_t terrain_program_ = 0;
  std::uint32_t color_program_ = 0;
    std::uint32_t sky_program_ = 0;
    std::uint32_t water_program_ = 0;
    std::uint32_t grass_program_ = 0;
    std::uint32_t line_vao_ = 0;
    std::uint32_t line_vbo_ = 0;
    std::uint32_t sky_vao_ = 0;
    std::uint32_t water_vao_ = 0;
    std::uint32_t water_vbo_ = 0;
    std::uint32_t grass_vao_ = 0;
    std::uint32_t grass_vbo_ = 0;
  float env_cam_[3] = {0, 0, 0};
  float env_sun_[3] = {0, 0, 0};
  float env_fog_[3] = {0.7f, 0.75f, 0.82f};
  float env_fog_range_[2] = {0, 0};

  // Shadow mapping.
  std::uint32_t shadow_fbo_ = 0;
  std::uint32_t shadow_array_ = 0;      // GL_TEXTURE_2D_ARRAY depth, kShadowCascades layers
  std::uint32_t shadow_depth_program_ = 0;
  int shadow_res_ = 2048;
  float shadow_vp_[kShadowCascades * 16] = {};
  float shadow_splits_[4] = {0, 0, 0, 0};
  int shadows_enabled_ = 0;

  // Offscreen scene capture + post-process.
  std::uint32_t scene_fbo_ = 0;
  std::uint32_t scene_color_ = 0;
  std::uint32_t scene_depth_rbo_ = 0;
  std::uint32_t post_program_ = 0;
  int scene_w_ = 0;
  int scene_h_ = 0;

  bool initialized_ = false;
};

}  // namespace bf2
