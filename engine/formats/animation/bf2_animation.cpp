#include "bf2_animation.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace bf2 {
namespace {

// Sequential little-endian reader with bounds checking.
class Reader {
public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  std::uint8_t byte() {
    require(1);
    return data_[pos_++];
  }
  std::uint16_t word() {
    require(2);
    std::uint16_t v = 0;
    std::memcpy(&v, data_ + pos_, 2);
    pos_ += 2;
    return v;
  }
  std::uint32_t dword() {
    require(4);
    std::uint32_t v = 0;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
  }
  float real() {
    require(4);
    float v = 0.f;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
  }
  void read(void* dst, std::size_t n) {
    require(n);
    std::memcpy(dst, data_ + pos_, n);
    pos_ += n;
  }
  std::size_t tell() const { return pos_; }
  std::size_t size() const { return size_; }

private:
  void require(std::size_t n) const {
    if (pos_ + n > size_) {
      throw std::runtime_error("Unexpected end of animation data");
    }
  }
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// Decode BF2's 16-bit fixed-point float. Precision 15 for rotations.
float decode_fixed16(std::uint16_t word, int precision) {
  const float mult = 32767.0f / static_cast<float>(1 << (15 - precision));
  int signed_word = word;
  if (signed_word > 32767) {
    signed_word -= 0xFFFF;
  }
  return static_cast<float>(signed_word) / mult;
}

}  // namespace

Skeleton SkeletonLoader::load_from_memory(const std::vector<std::uint8_t>& data) {
  Reader r(data.data(), data.size());
  Skeleton skeleton;
  skeleton.version = r.dword();
  const std::uint32_t node_count = r.dword();
  skeleton.nodes.resize(node_count);

  for (auto& node : skeleton.nodes) {
    const std::uint16_t name_len = r.word();
    if (name_len > 0) {
      std::string name(name_len, '\0');
      r.read(name.data(), name_len);
      if (!name.empty() && name.back() == '\0') {
        name.pop_back();
      }
      node.name = std::move(name);
    }
    node.parent = static_cast<std::int16_t>(r.word());
    // BF2 stores the conjugated rotation; invert (negate x,y,z) to recover it,
    // matching the reference importer's rot.invert() after load.
    node.rotation.x = -r.real();
    node.rotation.y = -r.real();
    node.rotation.z = -r.real();
    node.rotation.w = r.real();
    node.position.x = r.real();
    node.position.y = r.real();
    node.position.z = r.real();
  }

  return skeleton;
}

Skeleton SkeletonLoader::load_from_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Skeleton file not found: " + path);
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  return load_from_memory(data);
}

AnimationClip AnimationLoader::load_from_memory(const std::vector<std::uint8_t>& data) {
  Reader r(data.data(), data.size());
  AnimationClip clip;
  clip.version = r.dword();
  if (clip.version != 4) {
    throw std::runtime_error("Unsupported .baf version " + std::to_string(clip.version));
  }

  const std::uint16_t bone_num = r.word();
  clip.bone_count = bone_num;

  std::vector<int> bone_ids(bone_num);
  for (auto& id : bone_ids) {
    id = r.word();
  }

  clip.frame_count = static_cast<int>(r.dword());
  clip.precision = r.byte();

  clip.tracks.resize(bone_num);
  for (std::size_t b = 0; b < bone_ids.size(); ++b) {
    auto& track = clip.tracks[b];
    track.bone_id = bone_ids[b];
    track.frames.assign(static_cast<std::size_t>(clip.frame_count), AnimationFrame{});

    (void)r.word();  // per-bone data_size (redundant for reading)

    for (int channel = 1; channel <= 7; ++channel) {
      int cur_frame = 0;
      int data_left = static_cast<int>(r.word());
      while (data_left > 0) {
        const std::uint8_t head = r.byte();
        const bool rle = (head & 0x80) != 0;
        const int num_frames = head & 0x7F;
        const std::uint8_t next_header = r.byte();

        std::uint16_t value = 0;
        if (rle) {
          value = r.word();
        }
        for (int i = 0; i < num_frames; ++i) {
          if (!rle) {
            value = r.word();
          }
          if (cur_frame >= clip.frame_count) {
            throw std::runtime_error("Corrupted .baf: frame index out of range");
          }
          AnimationFrame& frame = track.frames[cur_frame];
          switch (channel) {
            case 1: frame.rotation.x = -decode_fixed16(value, 15); break;
            case 2: frame.rotation.y = -decode_fixed16(value, 15); break;
            case 3: frame.rotation.z = -decode_fixed16(value, 15); break;
            case 4: frame.rotation.w = decode_fixed16(value, 15); break;
            case 5: frame.position.x = decode_fixed16(value, clip.precision); break;
            case 6: frame.position.y = decode_fixed16(value, clip.precision); break;
            case 7: frame.position.z = decode_fixed16(value, clip.precision); break;
            default: break;
          }
          ++cur_frame;
        }
        data_left -= next_header;
      }
    }
  }

  return clip;
}

AnimationClip AnimationLoader::load_from_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Animation file not found: " + path);
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  return load_from_memory(data);
}

}  // namespace bf2
