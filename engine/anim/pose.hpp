#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "engine/formats/animation/bf2_animation.hpp"

namespace bf2 {

// World-space evaluation of a skeleton, optionally driven by an animation clip.
struct PosedSkeleton {
  std::vector<glm::mat4> world_transforms;  // one per skeleton node
  std::vector<glm::vec3> world_positions;   // convenience: translation of each node
};

// Compose local bone transforms (bind pose, or animation frame when clip != null)
// up the parent hierarchy into world space. Frame is clamped to the clip range.
PosedSkeleton pose_skeleton(const Skeleton& skeleton, const AnimationClip* clip, int frame);

}  // namespace bf2
