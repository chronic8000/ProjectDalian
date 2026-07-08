#include "dds_loader.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace bf2 {
namespace {

void unpack_rgb565(std::uint16_t c, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
  r = static_cast<std::uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
  g = static_cast<std::uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
  b = static_cast<std::uint8_t>((c & 0x1F) * 255 / 31);
}

void decode_dxt1_block(const std::uint8_t* block, std::uint8_t* out_rgba, int stride) {
  const std::uint16_t c0 = static_cast<std::uint16_t>(block[0] | (block[1] << 8));
  const std::uint16_t c1 = static_cast<std::uint16_t>(block[2] | (block[3] << 8));
  std::uint8_t colors[4][4];
  unpack_rgb565(c0, colors[0][0], colors[0][1], colors[0][2]);
  colors[0][3] = 255;
  unpack_rgb565(c1, colors[1][0], colors[1][1], colors[1][2]);
  colors[1][3] = 255;
  if (c0 > c1) {
    colors[2][0] = static_cast<std::uint8_t>((2 * colors[0][0] + colors[1][0]) / 3);
    colors[2][1] = static_cast<std::uint8_t>((2 * colors[0][1] + colors[1][1]) / 3);
    colors[2][2] = static_cast<std::uint8_t>((2 * colors[0][2] + colors[1][2]) / 3);
    colors[2][3] = 255;
    colors[3][0] = static_cast<std::uint8_t>((colors[0][0] + 2 * colors[1][0]) / 3);
    colors[3][1] = static_cast<std::uint8_t>((colors[0][1] + 2 * colors[1][1]) / 3);
    colors[3][2] = static_cast<std::uint8_t>((colors[0][2] + 2 * colors[1][2]) / 3);
    colors[3][3] = 255;
  } else {
    colors[2][0] = static_cast<std::uint8_t>((colors[0][0] + colors[1][0]) / 2);
    colors[2][1] = static_cast<std::uint8_t>((colors[0][1] + colors[1][1]) / 2);
    colors[2][2] = static_cast<std::uint8_t>((colors[0][2] + colors[1][2]) / 2);
    colors[2][3] = 255;
    colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
  }
  std::uint32_t indices = static_cast<std::uint32_t>(block[4]) | (static_cast<std::uint32_t>(block[5]) << 8) |
                          (static_cast<std::uint32_t>(block[6]) << 16) |
                          (static_cast<std::uint32_t>(block[7]) << 24);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const int idx = indices & 3;
      indices >>= 2;
      std::uint8_t* px = out_rgba + y * stride + x * 4;
      px[0] = colors[idx][0];
      px[1] = colors[idx][1];
      px[2] = colors[idx][2];
      px[3] = colors[idx][3];
    }
  }
}

void decode_dxt3_alpha_block(const std::uint8_t* block, std::uint8_t* out_alpha) {
  for (int i = 0; i < 8; ++i) {
    const std::uint8_t byte = block[i];
    out_alpha[i * 2] = static_cast<std::uint8_t>((byte & 0x0F) * 17);
    out_alpha[i * 2 + 1] = static_cast<std::uint8_t>(((byte >> 4) & 0x0F) * 17);
  }
}

void decode_dxt5_alpha_block(const std::uint8_t* block, std::uint8_t* out_alpha) {
  const std::uint8_t a0 = block[0];
  const std::uint8_t a1 = block[1];
  std::uint8_t alphas[8];
  alphas[0] = a0;
  alphas[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) {
      alphas[i + 1] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
    }
  } else {
    for (int i = 1; i < 5; ++i) {
      alphas[i + 1] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
    }
    alphas[6] = 0;
    alphas[7] = 255;
  }
  std::uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) {
    indices |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
  }
  for (int i = 0; i < 16; ++i) {
    out_alpha[i] = alphas[indices & 7];
    indices >>= 3;
  }
}

DdsTexture decode_blocks(const DdsTexture& src, DdsFormat block_format, int alpha_block_size) {
  DdsTexture out;
  out.width = src.width;
  out.height = src.height;
  out.format = DdsFormat::RGBA8;
  out.pixels.assign(static_cast<std::size_t>(src.width) * src.height * 4, 255);
  const int bw = static_cast<int>((src.width + 3) / 4);
  const int bh = static_cast<int>((src.height + 3) / 4);
  const int color_block_size = block_format == DdsFormat::DXT1 ? 8 : 16;
  const int block_stride = color_block_size + alpha_block_size;

  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      const std::size_t block_off =
          static_cast<std::size_t>(by * bw + bx) * static_cast<std::size_t>(block_stride);
      if (block_off + block_stride > src.pixels.size()) {
        continue;
      }
      const std::uint8_t* block = src.pixels.data() + block_off;
      std::uint8_t rgba[4 * 4 * 4]{};
      std::uint8_t alpha[16]{255, 255, 255, 255, 255, 255, 255, 255,
                             255, 255, 255, 255, 255, 255, 255, 255};
      if (alpha_block_size == 8) {
        decode_dxt3_alpha_block(block, alpha);
      } else if (alpha_block_size == 16) {
        decode_dxt5_alpha_block(block, alpha);
      }
      decode_dxt1_block(block + alpha_block_size, rgba, 16);
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          const int px = bx * 4 + x;
          const int py = by * 4 + y;
          if (px >= static_cast<int>(src.width) || py >= static_cast<int>(src.height)) {
            continue;
          }
          const std::size_t dst = (static_cast<std::size_t>(py) * src.width + px) * 4;
          const std::uint8_t* src_px = rgba + (y * 4 + x) * 4;
          out.pixels[dst] = src_px[0];
          out.pixels[dst + 1] = src_px[1];
          out.pixels[dst + 2] = src_px[2];
          out.pixels[dst + 3] = alpha[y * 4 + x];
        }
      }
    }
  }
  return out;
}

}  // namespace

DdsTexture DdsLoader::decode_to_rgba8(const DdsTexture& compressed) {
  if (compressed.format == DdsFormat::RGBA8) {
    return compressed;
  }
  if (compressed.format == DdsFormat::DXT1) {
    return decode_blocks(compressed, DdsFormat::DXT1, 0);
  }
  if (compressed.format == DdsFormat::DXT3) {
    return decode_blocks(compressed, DdsFormat::DXT3, 8);
  }
  if (compressed.format == DdsFormat::DXT5) {
    return decode_blocks(compressed, DdsFormat::DXT5, 16);
  }
  throw std::runtime_error("Unsupported DDS format for decode");
}

DdsTexture DdsLoader::load_from_memory(const std::vector<std::uint8_t>& data) {
  if (data.size() < 128 || std::memcmp(data.data(), "DDS ", 4) != 0) {
    throw std::runtime_error("Invalid DDS header");
  }

  DdsTexture texture;
  std::uint32_t height = 0;
  std::uint32_t width = 0;
  std::uint32_t pitch_or_linear = 0;
  std::uint32_t mip_map_count = 0;
  std::uint32_t four_cc = 0;

  std::memcpy(&height, data.data() + 12, 4);
  std::memcpy(&width, data.data() + 16, 4);
  std::memcpy(&pitch_or_linear, data.data() + 20, 4);
  std::memcpy(&mip_map_count, data.data() + 28, 4);
  std::memcpy(&four_cc, data.data() + 84, 4);

  texture.width = width;
  texture.height = height;
  texture.mip_count = mip_map_count > 0 ? mip_map_count : 1;

  const std::size_t header_size = 128;
  if (four_cc == 0) {
    texture.format = DdsFormat::RGBA8;
    texture.mip_count = 1;  // uncompressed path only uses level 0
    texture.pixels.assign(data.begin() + static_cast<std::ptrdiff_t>(header_size), data.end());
    return texture;
  }

  if (std::memcmp(&four_cc, "DXT1", 4) == 0) {
    texture.format = DdsFormat::DXT1;
  } else if (std::memcmp(&four_cc, "DXT3", 4) == 0) {
    texture.format = DdsFormat::DXT3;
  } else if (std::memcmp(&four_cc, "DXT5", 4) == 0) {
    texture.format = DdsFormat::DXT5;
  } else {
    texture.format = DdsFormat::Unknown;
  }

  texture.pixels.assign(data.begin() + static_cast<std::ptrdiff_t>(header_size), data.end());
  (void)pitch_or_linear;
  return texture;
}

DdsTexture DdsLoader::load_from_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("DDS file not found: " + path);
  }
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(data.data()), size);
  return load_from_memory(data);
}

}  // namespace bf2
