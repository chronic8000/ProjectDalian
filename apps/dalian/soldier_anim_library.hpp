#pragma once

#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/core/resource_manager.hpp"

#include <string>
#include <unordered_map>

namespace dalian {

// Loads all clips listed in soldiers/Common/Animations/AnimationSystem3p.inc.
class SoldierAnimLibrary {
public:
  bool load_from_inc(bf2::ResourceManager& resources,
                     const std::string& inc_vpath = "soldiers/common/animations/animationsystem3p.inc");

  bool has(const std::string& clip_name) const;
  const bf2::AnimationClip* get(const std::string& clip_name) const;
  std::size_t size() const { return clips_.size(); }
  const std::unordered_map<std::string, bf2::AnimationClip>& clips() const { return clips_; }

  // Common locomotion lookups (lowercase keys).
  const bf2::AnimationClip* stand() const;
  const bf2::AnimationClip* walk_forward() const;
  const bf2::AnimationClip* run_forward() const;
  const bf2::AnimationClip* crouch_still() const;
  const bf2::AnimationClip* fire() const;
  const bf2::AnimationClip* die() const;

  void fill_anim_set(struct SoldierAnimSet& out) const;

private:
  std::unordered_map<std::string, bf2::AnimationClip> clips_;
};

}  // namespace dalian
