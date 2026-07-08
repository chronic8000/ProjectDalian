#pragma once

// Cascaded Shadow Maps (CSM) scaffold.
//
// BF2 baked its shadows into UV5 lightmaps; a 2026 renderer wants real-time
// dynamic sun shadows instead. CSM splits the camera's view frustum into a few
// depth ranges ("cascades"), renders a shadow depth map for each from the sun's
// point of view, and samples the tightest cascade that covers each fragment.
// This gives crisp shadows up close and cheap, softer shadows in the distance.
//
// This header provides the *math and lifecycle* of a CSM system: practical split
// distances and, per cascade, a light-space view-projection matrix tightly
// fitted around that slice of the camera frustum. The GPU depth-pass rendering
// (allocating the depth textures/FBOs, drawing occluders into them, and sampling
// them in the surface shader with PCF) is the remaining integration step; the
// hooks below (`begin_cascade` / `end_cascade`) mark exactly where that plugs in.

#include <array>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace bf2 {

template <int kCascades = 4>
class CascadedShadowMaps {
 public:
  struct Cascade {
    glm::mat4 view_proj{1.0f};  // world -> light clip space for this slice
    float split_far = 0.f;      // view-space far distance covered by this cascade
    unsigned int depth_tex = 0; // GL depth texture (0 until the depth pass is wired)
  };

  int resolution() const { return resolution_; }
  void set_resolution(int px) { resolution_ = px; }
  float lambda() const { return lambda_; }
  void set_lambda(float l) { lambda_ = l; }  // 0 = uniform splits, 1 = logarithmic
  static constexpr int cascade_count() { return kCascades; }
  const Cascade& cascade(int i) const { return cascades_[i]; }

  // Practical split scheme (Nvidia): blend of logarithmic and uniform splits.
  void compute_splits(float near_plane, float far_plane) {
    for (int i = 0; i < kCascades; ++i) {
      const float p = static_cast<float>(i + 1) / static_cast<float>(kCascades);
      const float log_split = near_plane * std::pow(far_plane / near_plane, p);
      const float uni_split = near_plane + (far_plane - near_plane) * p;
      splits_[i] = lambda_ * log_split + (1.f - lambda_) * uni_split;
    }
  }

  // Recompute each cascade's light-space matrix so it tightly bounds that slice
  // of the camera frustum. `light_dir` points *from* the sun (i.e. the direction
  // sunlight travels). Call once per frame before the depth passes.
  void update(const glm::mat4& cam_view, float fov_y_rad, float aspect, float near_plane,
              float far_plane, const glm::vec3& light_dir) {
    compute_splits(near_plane, far_plane);
    const glm::mat4 inv_view = glm::inverse(cam_view);
    const glm::vec3 L = glm::normalize(light_dir);

    float last = near_plane;
    for (int c = 0; c < kCascades; ++c) {
      const float split_near = last;
      const float split_far = splits_[c];
      last = split_far;

      // Corners of this sub-frustum in world space.
      const float tan_v = std::tan(fov_y_rad * 0.5f);
      const float tan_h = tan_v * aspect;
      std::array<glm::vec3, 8> corners{};
      int k = 0;
      for (int zi = 0; zi < 2; ++zi) {
        const float z = (zi == 0) ? split_near : split_far;
        const float x = z * tan_h;
        const float y = z * tan_v;
        // View space: camera looks down -Z.
        corners[k++] = glm::vec3(inv_view * glm::vec4(-x, -y, -z, 1.f));
        corners[k++] = glm::vec3(inv_view * glm::vec4(x, -y, -z, 1.f));
        corners[k++] = glm::vec3(inv_view * glm::vec4(-x, y, -z, 1.f));
        corners[k++] = glm::vec3(inv_view * glm::vec4(x, y, -z, 1.f));
      }

      glm::vec3 center(0.f);
      for (const auto& p : corners) center += p;
      center /= 8.f;

      // Bounding sphere radius keeps the projection stable under rotation.
      float radius = 0.f;
      for (const auto& p : corners) radius = std::max(radius, glm::length(p - center));
      radius = std::ceil(radius * 16.f) / 16.f;  // snap to reduce shimmering

      const glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
      const glm::vec3 eye = center - L * (radius + texel_pad_);
      const glm::mat4 light_view = glm::lookAt(eye, center, up);
      glm::mat4 light_proj =
          glm::ortho(-radius, radius, -radius, radius, 0.f, 2.f * radius + texel_pad_);

      cascades_[c].view_proj = light_proj * light_view;
      cascades_[c].split_far = split_far;
    }
  }

  // Depth-pass hooks. Wire these to bind the cascade's FBO/viewport and restore
  // state once occluders have been drawn. Left as no-ops in the scaffold.
  void begin_cascade(int /*index*/) {}
  void end_cascade(int /*index*/) {}

 private:
  int resolution_ = 2048;
  float lambda_ = 0.75f;
  float texel_pad_ = 10.f;  // extra depth range so tall occluders aren't clipped
  std::array<float, kCascades> splits_{};
  std::array<Cascade, kCascades> cascades_{};
};

}  // namespace bf2
