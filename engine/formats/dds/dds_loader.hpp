#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf2 {

enum class DdsFormat { Unknown, RGBA8, DXT1, DXT3, DXT5 };

struct DdsTexture {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t mip_count = 1;  // number of mip levels stored in pixels
  DdsFormat format = DdsFormat::Unknown;
  std::vector<std::uint8_t> pixels;
};

class DdsLoader {
public:
  static DdsTexture load_from_memory(const std::vector<std::uint8_t>& data);
  static DdsTexture load_from_file(const std::string& path);
  // Decompress DXT1/DXT3/DXT5 mip0 to tight RGBA8 row-major pixels.
  static DdsTexture decode_to_rgba8(const DdsTexture& compressed);
};

}  // namespace bf2
