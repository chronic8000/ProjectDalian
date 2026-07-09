#include "soldier_anim.hpp"

#include <glm/glm.hpp>

namespace dalian {

const bf2::AnimationClip* select_soldier_clip(const SoldierAnimSet& set, const SoldierAnimState& st,
                                              float& out_rate) {
  out_rate = 1.f;
  if (!st.alive && set.death) {
    out_rate = 1.f;
    return set.death;
  }
  if (st.pose == SoldierPose::Reload && set.reload) {
    out_rate = 1.f;
    return set.reload;
  }
  if (st.pose == SoldierPose::Fire && set.fire) {
    out_rate = 1.2f;
    return set.fire;
  }
  if (st.pose == SoldierPose::Prone && set.prone) {
    out_rate = st.moving ? glm::clamp(st.move_speed / 0.8f, 0.5f, 1.2f) : 1.f;
    return set.prone;
  }
  if (st.pose == SoldierPose::Crouch && set.crouch) {
    out_rate = st.moving ? glm::clamp(st.move_speed / 1.2f, 0.5f, 1.4f) : 1.f;
    return set.crouch;
  }
  if (st.moving) {
    if (st.move_speed > 2.2f && set.run) {
      out_rate = glm::clamp(st.move_speed / 5.5f, 0.6f, 1.8f);
      return set.run;
    }
    if (set.walk) {
      out_rate = glm::clamp(st.move_speed / 1.5f, 0.6f, 1.6f);
      return set.walk;
    }
  }
  out_rate = 1.f;
  return set.stand;
}

}  // namespace dalian
