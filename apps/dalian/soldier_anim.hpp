#pragma once

#include "engine/formats/animation/bf2_animation.hpp"

namespace dalian {

enum class SoldierPose : std::uint8_t {
  Stand,
  Walk,
  Run,
  Crouch,
  Prone,
  Fire,
  Reload,
  Death,
};

struct SoldierAnimSet {
  const bf2::AnimationClip* stand = nullptr;
  const bf2::AnimationClip* walk = nullptr;
  const bf2::AnimationClip* run = nullptr;
  const bf2::AnimationClip* crouch = nullptr;
  const bf2::AnimationClip* prone = nullptr;
  const bf2::AnimationClip* fire = nullptr;
  const bf2::AnimationClip* reload = nullptr;
  const bf2::AnimationClip* death = nullptr;
};

struct SoldierAnimState {
  SoldierPose pose = SoldierPose::Stand;
  float time = 0.f;
  float rate = 1.f;
  bool moving = false;
  float move_speed = 0.f;
  bool alive = true;
};

// Pick clip + playback rate from locomotion speed and pose.
const bf2::AnimationClip* select_soldier_clip(const SoldierAnimSet& set, const SoldierAnimState& st,
                                              float& out_rate);

}  // namespace dalian
