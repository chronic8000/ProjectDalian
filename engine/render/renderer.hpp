#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

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
  std::uint32_t specular_tex = 0;  // BF2 *_s gloss map (0 = use diffuse alpha)
  bool cutout = false;  // base texture is an alpha cutout mask (foliage/fence)
  bool track_uv = false;  // scroll base UVs when the vehicle moves (tank treads)
  bool track_uv_axis_v = true;  // false = scroll U, true = scroll V (BF2 TranslationMax)
  // When set, scroll wraps inside the atlas tread strip instead of GL_REPEAT
  // into neighboring hull camo (which reads as vertical smears on the belt).
  bool track_uv_wrap_strip = false;
  float track_strip_umin = 0.f;
  float track_strip_umax = 1.f;
  float track_strip_vmin = 0.f;
  float track_strip_vmax = 1.f;
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
// One textured run of a skinned mesh (e.g. body vs. head). `tex` is filled in by
// the caller after upload by resolving `SkinnedSubmesh::diffuse_map`.
struct GpuSkinnedSubmesh {
  std::uint32_t index_offset = 0;
  std::uint32_t index_count = 0;
  std::uint32_t tex = 0;
};

struct GpuSkinnedMesh {
  std::uint32_t vao = 0;
  std::uint32_t vbo = 0;
  std::uint32_t ebo = 0;
  std::uint32_t index_count = 0;
  std::vector<GpuSkinnedSubmesh> submeshes;
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
  // Upload a DDS to the GPU. Set `mipmaps=false` for alpha-tested atlases (e.g.
  // grass) where mip averaging melts blades+background into opaque grey blobs.
  std::uint32_t upload_texture(const DdsTexture& texture, bool mipmaps = true);
  void destroy_texture(std::uint32_t texture);
  // Uploads world-space textured geometry; the returned mesh's submeshes come
  // from data.submeshes with texture ids filled in by the caller.
  GpuTexturedMesh upload_textured(const TexturedMeshData& data);
  // mvp = viewProj * model (column-major). model is used to rotate normals for
  // lighting; pass nullptr when vertices are already in world space. When
  // obj_lightmap != 0 the mesh's lightmap UV is sampled from that atlas texture,
  // transformed by lm_xform (xy = uv scale, zw = uv offset) -- BF2's per-instance
  // baked object lighting.
  // cull_backfaces: enable for closed solids (vehicles) so their interior back
  // faces aren't visible through openings. Leave off for static objects, which
  // include double-sided foliage/fences that must render from both sides.
  // alpha_mode: 0 opaque; 1 = alpha-blend using the base texture's alpha channel
  // (rotor blur); 2 = cutout (per-submesh); 3 = road soft edge (vertex alpha).
  void draw_textured(const GpuTexturedMesh& mesh, const float* mvp, const float* model = nullptr,
                     std::uint32_t obj_lightmap = 0, const float* lm_xform = nullptr,
                     bool cull_backfaces = false, int alpha_mode = 0, float uv_scroll_u = 0.f,
                     bool scroll_all_uv = false);
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

  // Camera-facing additive/textured sprites (smoke, fire, tracers, baselights).
  // cam_right/cam_up must be orthonormal view axes (not world X/Y) or sprites
  // collapse to thin vertical lines when viewed edge-on.
  struct BillboardParticle {
    float x, y, z;
    float size;
    float r, g, b, a;
    float kind;  // 0=smoke, 1=exhaust, 2=glow/afterburner, 3=explosion_fire
  };
  void draw_billboards(const float* view_proj, const float* cam_right3, const float* cam_up3,
                       const BillboardParticle* parts, int count, std::uint32_t tex,
                       bool additive = true, bool fire_mode = false);

  // ---- 2D UI (menus / HUD) --------------------------------------------------
  // Screen-space, top-left origin (y grows downward), alpha-blended, no depth.
  // Call begin_ui() once, issue ui_rect/ui_text, then end_ui() to restore state.
  // Prefer begin_ui(SDL_Window*) — it queries the real GL drawable size every frame.
  void begin_ui(SDL_Window* window);
  void begin_ui(int width, int height);
  void ui_rect(float x, float y, float w, float h, float r, float g, float b, float a);
  // Draws `text` with its top-left at (x,y). `scale` multiplies the built-in font
  // (scale 2 ~= 16px tall). Letter spacing is set via stb_easy_font_spacing so
  // width metrics match the drawn glyphs. Use ui_text_width / ui_text_height to lay out.
  void ui_text(float x, float y, float scale, const char* text, float r, float g, float b,
               float a);
  float ui_text_width(const char* text, float scale) const;
  // Single-line glyph height (scale 1 ≈ 12px). Prefer the text overload for '\n'.
  float ui_text_height(float scale) const;
  float ui_text_height(const char* text, float scale) const;
  // Upload an RGBA8 image (4 bytes/pixel, top-left origin) for UI drawing.
  std::uint32_t upload_rgba_texture(int width, int height, const std::uint8_t* rgba);
  // Draw a texture across the full framebuffer (object-fit: cover).
  void ui_image_cover(std::uint32_t texture, int img_w, int img_h, float alpha = 1.f);
  // Semi-transparent dim over the full framebuffer (e.g. behind menu panels).
  void ui_dim_framebuffer(float alpha);
  // Map screen pixels to the fixed 1600x900 design space used by menu layout.
  void ui_unproject(int screen_x, int screen_y, float& design_x, float& design_y) const;
  float ui_scale() const { return ui_scale_; }
  void end_ui();

  // Per-frame environment applied to terrain/object/water shaders: camera world
  // position (for fog distance), sun/moon direction (light travel direction), fog
  // colour, and fog start/end distances (fog_end <= 0 disables fog).
  // sun_color3 / ambient_scale tint direct light and fill (night moonlight).
  void set_environment(const float* cam_pos3, const float* sun_dir3, const float* fog_color3,
                       float fog_start, float fog_end, const float* sun_color3 = nullptr,
                       float ambient_scale = 1.f);

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
  // Resolve the offscreen scene to the default framebuffer through HDR tone map,
  // bloom, optional SSAO, and FPV degrade. near_z/far_z reconstruct view depth.
  void present_scene(float degrade, float time_seconds, float near_z = 0.2f,
                     float far_z = 8000.f, int output_w = 0, int output_h = 0);

  void set_bloom(bool enabled, float intensity);
  void set_ssao(bool enabled);
  void set_hdr(bool enabled);
  void set_hdr_exposure(float exposure);
  // Always-on output gain (SDR capture / YouTube). 1 = neutral, ~1.2–1.4 if OBS looks dark.
  void set_output_brightness(float brightness);
  void set_shadows_enabled(bool enabled);
  bool reload_shadow_res(int resolution);
  void set_anisotropic(int level);
  // Positive bias = blurrier/cheaper mips (good with HD textures); negative = sharper.
  void set_mip_lod_bias(float bias);
  // Cap longest edge at upload (0 = unlimited). Skips oversized DDS mips — biggest
  // HD-pack VRAM lever. Requires map reload to affect already-cached textures.
  void set_max_texture_size(int max_edge);
  int max_texture_size() const { return max_texture_size_; }
  // 0 = bilinear stretch, 1 = FSR1 EASU+RCAS, 2 = Auto (→ FSR1 on OpenGL).
  void set_upscale_mode(int mode);
  // RCAS sharpness in AMD "stops" (0 = max sharp, ~2 = soft). Typical 0.2.
  void set_fsr_sharpness(float stops);
  // When false (HDR tonemap off), scene/bloom use RGBA8 — required on many
  // Intel/old laptop GPUs where RGB16F FBOs render as green/magenta garbage.
  bool using_float_color() const { return use_float_color_; }

  // Full-screen sky. Samples Skydome.skyTexture when provided; otherwise a
  // vertical gradient. Night maps draw a moon disc from flareDirection.
  void draw_sky(const float* inv_view_proj, const float* cam_pos3, const float* sky_color3,
                const float* horizon_color3, std::uint32_t sky_tex = 0,
                std::uint32_t cloud_tex = 0, float cloud_scroll_u = 0.f,
                float cloud_scroll_v = 0.f, float cloud_strength = 0.55f,
                const float* sun_color3 = nullptr, int is_night = 0,
                const float* moon_dir3 = nullptr, const float* moon_color3 = nullptr,
                float dome_rotation_deg = 0.f);

  // Translucent animated water plane at world height level_y, centred on the
  // camera's XZ and extending half_extent metres in every direction.
  void draw_water(const float* view_proj, float level_y, float cam_x, float cam_z, float half_extent,
                  float time_seconds, const float* water_color3,
                  const float* water_specular3 = nullptr, float specular_power = 12.f,
                  const float* water_fog_color3 = nullptr, float sun_intensity = 1.f,
                  const float* sun_color3 = nullptr);

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
  std::uint32_t fallback_tex_ = 0;  // neutral grey, used when a submesh texture is missing
  std::uint32_t terrain_program_ = 0;
  std::uint32_t color_program_ = 0;
    std::uint32_t sky_program_ = 0;
    std::uint32_t water_program_ = 0;
    std::uint32_t grass_program_ = 0;
    std::uint32_t line_vao_ = 0;
    std::uint32_t line_vbo_ = 0;
    std::uint32_t particle_program_ = 0;
    std::uint32_t particle_vao_ = 0;
    std::uint32_t particle_vbo_ = 0;
    std::uint32_t ui_program_ = 0;
    std::uint32_t ui_tex_program_ = 0;
    std::uint32_t ui_vao_ = 0;
    std::uint32_t ui_vbo_ = 0;
    std::uint32_t ui_tex_vao_ = 0;
    std::uint32_t ui_tex_vbo_ = 0;
    float ui_proj_[16] = {};
    float ui_scale_ = 1.f;
    float ui_off_x_ = 0.f;
    float ui_off_y_ = 0.f;
    SDL_Window* sdl_window_ = nullptr;
    int ui_fb_w_ = 0;
    int ui_fb_h_ = 0;
    int vp_w_ = 1600;
    int vp_h_ = 900;
    std::uint32_t sky_vao_ = 0;
    std::uint32_t water_vao_ = 0;
    std::uint32_t water_vbo_ = 0;
    std::uint32_t grass_vao_ = 0;
    std::uint32_t grass_vbo_ = 0;
  float env_cam_[3] = {0, 0, 0};
  float env_sun_[3] = {0, 0, 0};
  float env_sun_color_[3] = {1.f, 0.96f, 0.88f};
  float env_ambient_scale_ = 1.f;
  float env_fog_[3] = {0.7f, 0.75f, 0.82f};
  float env_fog_range_[2] = {0, 0};

  // Shadow mapping.
  std::uint32_t shadow_fbo_ = 0;
  std::uint32_t shadow_array_ = 0;      // GL_TEXTURE_2D_ARRAY depth, kShadowCascades layers
  std::uint32_t shadow_depth_program_ = 0;
  int shadow_res_ = 4096;
  float shadow_vp_[kShadowCascades * 16] = {};
  float shadow_splits_[4] = {0, 0, 0, 0};
  int shadows_enabled_ = 0;

  // Offscreen scene capture + post-process (HDR colour + sampleable depth).
  std::uint32_t scene_fbo_ = 0;
  std::uint32_t scene_color_ = 0;
  std::uint32_t scene_depth_tex_ = 0;  // sampleable depth for SSAO
  std::uint32_t post_program_ = 0;
  int scene_w_ = 0;
  int scene_h_ = 0;

  // Bloom: half-res bright-pass + separable Gaussian ping-pong (HDR RGB16F).
  std::uint32_t bright_program_ = 0;
  std::uint32_t blur_program_ = 0;
  std::uint32_t bloom_fbo_[2] = {0, 0};
  std::uint32_t bloom_tex_[2] = {0, 0};
  int bloom_w_ = 0;
  int bloom_h_ = 0;
  bool bloom_float_ = false;  // last bloom allocation used float colour
  bool bloom_enabled_ = true;
  float bloom_intensity_ = 0.28f;
  float bloom_threshold_ = 1.05f;
  int anisotropic_ = 4;
  float mip_lod_bias_ = 0.f;
  int max_texture_size_ = 2048;  // 0 = unlimited; default caps 4K remasters
  int upscale_mode_ = 1;  // SpatialFsr by default when render_scale < 1
  float fsr_sharpness_ = 0.2f;
  std::vector<std::uint32_t> tracked_textures_;

  // LDR resolve + FSR1 intermediates (post → EASU → RCAS).
  std::uint32_t resolve_fbo_ = 0;
  std::uint32_t resolve_tex_ = 0;
  int resolve_w_ = 0;
  int resolve_h_ = 0;
  std::uint32_t easu_fbo_ = 0;
  std::uint32_t easu_tex_ = 0;
  int easu_w_ = 0;
  int easu_h_ = 0;
  std::uint32_t easu_program_ = 0;
  std::uint32_t rcas_program_ = 0;

  // SSAO: half-res AO from scene depth, blurred, multiplied in post.
  std::uint32_t ssao_program_ = 0;
  std::uint32_t ssao_blur_program_ = 0;
  std::uint32_t ssao_fbo_[2] = {0, 0};
  std::uint32_t ssao_tex_[2] = {0, 0};
  int ssao_w_ = 0;
  int ssao_h_ = 0;
  bool ssao_enabled_ = true;
  bool hdr_enabled_ = false;
  float hdr_exposure_ = 0.55f;
  float output_brightness_ = 1.f;
  bool use_float_color_ = false;   // RGB16F only when HDR tonemap is on + GPU OK
  bool float_color_ok_ = true;     // probed at init
  int scene_color_fmt_ = 0;        // tracks last allocated internal format

  bool initialized_ = false;
};

}  // namespace bf2
