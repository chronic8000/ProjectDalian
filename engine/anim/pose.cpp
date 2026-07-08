#include "pose.hpp"

#include <algorithm>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace bf2 {

PosedSkeleton pose_skeleton(const Skeleton& skeleton, const AnimationClip* clip, int frame) {
  PosedSkeleton out;
  const std::size_t node_count = skeleton.nodes.size();
  out.world_transforms.assign(node_count, glm::mat4(1.0f));
  out.world_positions.assign(node_count, glm::vec3(0.0f));

  // Map animated bone_id -> track index for O(1) override lookup.
  std::unordered_map<int, const AnimationBoneTrack*> tracks;
  if (clip) {
    for (const auto& track : clip->tracks) {
      if (!track.frames.empty()) {
        tracks[track.bone_id] = &track;
      }
    }
  }

  for (std::size_t i = 0; i < node_count; ++i) {
    const auto& node = skeleton.nodes[i];

    // GLM quaternion constructor order is (w, x, y, z).
    glm::quat rot(node.rotation.w, node.rotation.x, node.rotation.y, node.rotation.z);
    glm::vec3 pos(node.position.x, node.position.y, node.position.z);

    if (const auto it = tracks.find(static_cast<int>(i)); it != tracks.end()) {
      const auto& frames = it->second->frames;
      const int f = std::clamp(frame, 0, static_cast<int>(frames.size()) - 1);
      const auto& kf = frames[f];
      rot = glm::quat(kf.rotation.w, kf.rotation.x, kf.rotation.y, kf.rotation.z);
      pos = glm::vec3(kf.position.x, kf.position.y, kf.position.z);
    }

    const glm::mat4 local = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot);
    glm::mat4 world = local;
    if (node.parent >= 0 && node.parent < static_cast<int>(i)) {
      world = out.world_transforms[static_cast<std::size_t>(node.parent)] * local;
    }
    out.world_transforms[i] = world;
    out.world_positions[i] = glm::vec3(world[3]);
  }

  return out;
}

}  // namespace bf2
