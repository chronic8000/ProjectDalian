#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/formats/mesh/bf2_mesh.hpp"

namespace bf2 {

struct Quat {
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  float w = 1.f;
};

struct SkeletonNode {
  std::string name;
  int parent = -1;
  Quat rotation{};
  Float3 position{};
};

struct Skeleton {
  std::uint32_t version = 0;
  std::vector<SkeletonNode> nodes;
};

struct AnimationFrame {
  Quat rotation{};
  Float3 position{};
};

struct AnimationBoneTrack {
  int bone_id = 0;
  std::vector<AnimationFrame> frames;
};

struct AnimationClip {
  std::uint32_t version = 0;
  int bone_count = 0;
  int frame_count = 0;
  std::uint8_t precision = 0;
  std::vector<AnimationBoneTrack> tracks;
};

class SkeletonLoader {
public:
  static Skeleton load_from_memory(const std::vector<std::uint8_t>& data);
  static Skeleton load_from_file(const std::string& path);
};

class AnimationLoader {
public:
  static AnimationClip load_from_memory(const std::vector<std::uint8_t>& data);
  static AnimationClip load_from_file(const std::string& path);
};

}  // namespace bf2
