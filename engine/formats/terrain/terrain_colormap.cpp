#include "terrain_colormap.hpp"

#include "engine/formats/terrain/terrain_con_parser.hpp"
#include "engine/formats/dds/dds_loader.hpp"

#include <GL/glew.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool discover_tile_grid(ResourceManager& resources, const std::string& folder, int& rows,
                      int& cols) {
  rows = 0;
  cols = 0;
  const std::string prefix = lower(folder) + "/tx";
  const std::regex pattern(R"(tx(\d+)x(\d+)\.dds$)", std::regex::icase);
  for (const auto& path : resources.archives().list(prefix)) {
    std::smatch m;
    if (!std::regex_search(path, m, pattern)) {
      continue;
    }
    // BF2 tiles are named tx<COL>x<ROW>: first index = column (X, increases
    // rightward), second index = row (Y, increases downward).
    const int x = std::stoi(m[1].str());
    const int y = std::stoi(m[2].str());
    cols = std::max(cols, x + 1);
    rows = std::max(rows, y + 1);
  }
  return rows > 0 && cols > 0;
}

std::string tile_path(const std::string& folder, int row, int col, const char* suffix = "") {
  char buf[160];
  // File name is tx<COL>x<ROW> (first=column/X, second=row/Y).
  std::snprintf(buf, sizeof(buf), "%s/tx%02dx%02d%s.dds", lower(folder).c_str(), col, row, suffix);
  return buf;
}

std::uint32_t upload_rgba_texture(const DdsTexture& rgba, bool tiling = false) {
  if (rgba.width == 0 || rgba.height == 0 || rgba.pixels.empty()) {
    return 0;
  }
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  const GLint wrap = tiling ? GL_REPEAT : GL_CLAMP_TO_EDGE;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
  // Trilinear + anisotropic so the low-res colormap doesn't shimmer/alias at
  // grazing angles across dunes and distant slopes.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(rgba.width),
               static_cast<GLsizei>(rgba.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.pixels.data());
  glGenerateMipmap(GL_TEXTURE_2D);
  if (GLEW_EXT_texture_filter_anisotropic) {
    GLfloat max_aniso = 1.f;
    glGetFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &max_aniso);
    const GLfloat aniso = max_aniso < 8.f ? max_aniso : 8.f;
    glTexParameterf(GL_TEXTURE_2D, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, aniso);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  return id;
}

// mean_fill: fill missing cells with the average colour of present tiles (good
// for the colormap). When false, missing cells are left black (good for blend
// masks, where "no tile" means "no blend").
bool stitch_tiles(const std::vector<DdsTexture>& tiles, int rows, int cols, DdsTexture& out,
                  bool mean_fill = true) {
  if (tiles.empty() || rows <= 0 || cols <= 0) {
    return false;
  }
  // Reference tile size = first non-empty tile (BF2 omits tiles for uniform /
  // out-of-bounds regions, so some grid slots can be empty).
  std::uint32_t tw = 0;
  std::uint32_t th = 0;
  for (const auto& t : tiles) {
    if (t.width > 0 && t.height > 0) {
      tw = t.width;
      th = t.height;
      break;
    }
  }
  if (tw == 0 || th == 0) {
    return false;
  }
  out.width = tw * static_cast<std::uint32_t>(cols);
  out.height = th * static_cast<std::uint32_t>(rows);
  out.format = DdsFormat::RGBA8;
  out.pixels.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);

  const int n_cells = rows * cols;
  std::vector<DdsTexture> decoded(static_cast<std::size_t>(n_cells));
  std::vector<std::array<float, 3>> cell_col(static_cast<std::size_t>(n_cells), {0, 0, 0});
  std::vector<bool> present(static_cast<std::size_t>(n_cells), false);

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const std::size_t idx = static_cast<std::size_t>(r * cols + c);
      if (idx >= tiles.size() || tiles[idx].width == 0 || tiles[idx].height == 0) {
        continue;
      }
      DdsTexture rgba = DdsLoader::decode_to_rgba8(tiles[idx]);
      const std::size_t need = static_cast<std::size_t>(tw) * th * 4;
      if (rgba.width != tw || rgba.height != th || rgba.pixels.size() < need) {
        continue;  // odd-sized / undecodable tile -> skip rather than crash
      }
      std::uint64_t sr = 0, sg = 0, sb = 0, sn = 0;
      for (std::size_t p = 0; p + 3 < rgba.pixels.size(); p += 4) {
        sr += rgba.pixels[p];
        sg += rgba.pixels[p + 1];
        sb += rgba.pixels[p + 2];
        ++sn;
      }
      if (sn) {
        cell_col[idx] = {static_cast<float>(sr) / sn, static_cast<float>(sg) / sn,
                         static_cast<float>(sb) / sn};
      }
      present[idx] = true;
      decoded[idx] = std::move(rgba);
    }
  }

  // For the colormap, give each MISSING cell a colour propagated from its filled
  // neighbours (iterative dilation) instead of one global average. Combined with
  // the continuous detail grit, this makes the flat "solid patch" cells blend
  // into their surroundings so tile joins stop reading as hard rectangles. Masks
  // (mean_fill == false) keep their black "no blend" fill.
  if (mean_fill) {
    std::vector<bool> known = present;
    bool changed = true;
    while (changed) {
      changed = false;
      for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
          const std::size_t idx = static_cast<std::size_t>(r * cols + c);
          if (known[idx]) {
            continue;
          }
          float ar = 0, ag = 0, ab = 0;
          int an = 0;
          const int dr[4] = {-1, 1, 0, 0};
          const int dc[4] = {0, 0, -1, 1};
          for (int k = 0; k < 4; ++k) {
            const int nr = r + dr[k];
            const int nc = c + dc[k];
            if (nr < 0 || nc < 0 || nr >= rows || nc >= cols) {
              continue;
            }
            const std::size_t nidx = static_cast<std::size_t>(nr * cols + nc);
            if (!known[nidx]) {
              continue;
            }
            ar += cell_col[nidx][0];
            ag += cell_col[nidx][1];
            ab += cell_col[nidx][2];
            ++an;
          }
          if (an > 0) {
            cell_col[idx] = {ar / an, ag / an, ab / an};
            changed = true;
          }
        }
      }
      // Commit this ring so newly-coloured cells feed the next pass.
      for (int i = 0; i < n_cells; ++i) {
        if (!known[i] && (cell_col[i][0] != 0 || cell_col[i][1] != 0 || cell_col[i][2] != 0)) {
          known[i] = true;
        }
      }
    }
  }

  // Helper: an edge sample for a neighbour cell. If the neighbour is a real
  // tile, read its facing edge pixel; otherwise fall back to the neighbour's
  // propagated flat colour. Returns false when there is no neighbour at all.
  auto neighbour_edge = [&](int nr, int nc, std::uint32_t x, std::uint32_t y, bool horizontal,
                            bool far_side, float out_rgb[3]) -> bool {
    if (nr < 0 || nc < 0 || nr >= rows || nc >= cols) {
      return false;
    }
    const std::size_t nidx = static_cast<std::size_t>(nr * cols + nc);
    if (present[nidx]) {
      const auto& px = decoded[nidx].pixels;
      std::uint32_t sx = 0, sy = 0;
      if (horizontal) {
        sx = far_side ? 0u : (tw - 1);  // right neighbour -> its col 0; left -> col tw-1
        sy = y;
      } else {
        sx = x;
        sy = far_side ? 0u : (th - 1);  // bottom neighbour -> row 0; top -> row th-1
      }
      const std::size_t p = (static_cast<std::size_t>(sy) * tw + sx) * 4;
      out_rgb[0] = px[p];
      out_rgb[1] = px[p + 1];
      out_rgb[2] = px[p + 2];
      return true;
    }
    out_rgb[0] = cell_col[nidx][0];
    out_rgb[1] = cell_col[nidx][1];
    out_rgb[2] = cell_col[nidx][2];
    return true;
  };

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const std::size_t idx = static_cast<std::size_t>(r * cols + c);
      if (present[idx]) {
        const DdsTexture& rgba = decoded[idx];
        for (std::uint32_t y = 0; y < th; ++y) {
          const std::size_t dst_row =
              (static_cast<std::size_t>(r) * th + y) * out.width + static_cast<std::size_t>(c) * tw;
          const std::size_t src_row = static_cast<std::size_t>(y) * tw;
          std::copy_n(rgba.pixels.begin() + src_row * 4, static_cast<std::size_t>(tw) * 4,
                      out.pixels.begin() + dst_row * 4);
        }
        continue;
      }
      // Missing cell (colormap): fill with a per-pixel gradient that matches
      // each neighbour's facing edge, so there is no hard step on any side. For
      // masks (all-zero fill) this collapses to zeros.
      const float fw = tw > 1 ? static_cast<float>(tw - 1) : 1.f;
      const float fh = th > 1 ? static_cast<float>(th - 1) : 1.f;
      for (std::uint32_t y = 0; y < th; ++y) {
        std::size_t p =
            ((static_cast<std::size_t>(r) * th + y) * out.width + static_cast<std::size_t>(c) * tw) *
            4;
        for (std::uint32_t x = 0; x < tw; ++x, p += 4) {
          float acc[3] = {0, 0, 0};
          float wsum = 0.f;
          float e[3];
          // Horizontal interpolation (left <-> right).
          float lv[3], rv[3];
          const bool hasL = neighbour_edge(r, c - 1, x, y, true, false, lv);
          const bool hasR = neighbour_edge(r, c + 1, x, y, true, true, rv);
          if (hasL || hasR) {
            const float tx = x / fw;
            for (int k = 0; k < 3; ++k) {
              const float a = hasL ? lv[k] : rv[k];
              const float b = hasR ? rv[k] : lv[k];
              e[k] = a * (1.f - tx) + b * tx;
            }
            const float w = 1.f;
            acc[0] += e[0] * w;
            acc[1] += e[1] * w;
            acc[2] += e[2] * w;
            wsum += w;
          }
          // Vertical interpolation (top <-> bottom).
          float tv[3], bv[3];
          const bool hasT = neighbour_edge(r - 1, c, x, y, false, false, tv);
          const bool hasB = neighbour_edge(r + 1, c, x, y, false, true, bv);
          if (hasT || hasB) {
            const float ty = y / fh;
            for (int k = 0; k < 3; ++k) {
              const float a = hasT ? tv[k] : bv[k];
              const float b = hasB ? bv[k] : tv[k];
              e[k] = a * (1.f - ty) + b * ty;
            }
            const float w = 1.f;
            acc[0] += e[0] * w;
            acc[1] += e[1] * w;
            acc[2] += e[2] * w;
            wsum += w;
          }
          if (wsum > 0.f) {
            out.pixels[p] = static_cast<std::uint8_t>(acc[0] / wsum);
            out.pixels[p + 1] = static_cast<std::uint8_t>(acc[1] / wsum);
            out.pixels[p + 2] = static_cast<std::uint8_t>(acc[2] / wsum);
          } else {
            out.pixels[p] = static_cast<std::uint8_t>(cell_col[idx][0]);
            out.pixels[p + 1] = static_cast<std::uint8_t>(cell_col[idx][1]);
            out.pixels[p + 2] = static_cast<std::uint8_t>(cell_col[idx][2]);
          }
          out.pixels[p + 3] = 255;
        }
      }
    }
  }
  return true;
}

// Load the first detail ground texture that resolves from a candidate list
// (decoded to RGBA8). BF2 shares these under Common_client.zip's
// Terrain/Textures/Detail/ folder.
DdsTexture load_first_detail(ResourceManager& resources,
                             const std::vector<std::string>& candidates) {
  for (const auto& path : candidates) {
    try {
      DdsTexture t = DdsLoader::decode_to_rgba8(resources.load_texture(path));
      if (t.width > 0 && !t.pixels.empty()) {
        return t;
      }
    } catch (...) {
    }
  }
  return DdsTexture{};
}

std::vector<DdsTexture> load_tile_grid(ResourceManager& resources, const std::string& folder,
                                      int rows, int cols, const char* suffix = "") {
  std::vector<DdsTexture> tiles;
  tiles.reserve(static_cast<std::size_t>(rows) * cols);
  int loaded = 0;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      // Missing tiles are expected (sparse grids); record an empty placeholder so
      // the grid stays aligned and stitching can fill the gap.
      try {
        tiles.push_back(resources.load_texture(tile_path(folder, r, c, suffix)));
        ++loaded;
      } catch (...) {
        tiles.push_back(DdsTexture{});
      }
    }
  }
  if (loaded == 0) {
    return {};
  }
  return tiles;
}

}  // namespace

float parse_heightmap_xz_scale(const std::string& heightdata_script, float default_xz) {
  std::istringstream in(heightdata_script);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;
    if (cmd != "heightmap.setScale") {
      continue;
    }
    std::string triple;
    ls >> triple;
    const auto s1 = triple.find('/');
    if (s1 == std::string::npos) {
      break;
    }
    try {
      return std::stof(triple.substr(0, s1));
    } catch (...) {
      break;
    }
  }
  return default_xz;
}

TerrainVisualConfig TerrainColormapLoader::parse_terrain_con(const std::string& script) {
  TerrainConParser parser;
  parser.parse(script);
  const TerrainConData& d = parser.data();

  auto folder_from_prefix = [](const std::string& prefix) -> std::string {
    if (prefix.size() >= 3 && prefix.compare(prefix.size() - 3, 3, "/tx") == 0) {
      return prefix.substr(0, prefix.size() - 3);
    }
    return prefix;
  };

  TerrainVisualConfig cfg;
  cfg.colormap_folder = folder_from_prefix(d.colormap_tile_prefix);
  cfg.lightmap_folder = folder_from_prefix(d.lightmap_tile_prefix);
  cfg.detail_folder = folder_from_prefix(d.detailmap_tile_prefix);
  cfg.tile_size = d.patch_colormap_size;
  return cfg;
}

TerrainGroundAtlases TerrainColormapLoader::build_atlases(ResourceManager& resources,
                                                         const TerrainVisualConfig& cfg) {
  TerrainGroundAtlases out;
  int rows = cfg.tile_rows;
  int cols = cfg.tile_cols;
  if (rows <= 0 || cols <= 0) {
    if (!discover_tile_grid(resources, cfg.colormap_folder, rows, cols)) {
      return out;
    }
  }

  const auto color_tiles = load_tile_grid(resources, cfg.colormap_folder, rows, cols);
  if (color_tiles.empty() || !stitch_tiles(color_tiles, rows, cols, out.colormap, /*mean_fill=*/true)) {
    return {};
  }

  // BF2 terrain lightmaps are not a simple RGB multiply (they encode sun/shadow
  // and sky-ambient), and multiplying them casts the desert cyan/green. The
  // colormap already has baked lighting, so keep lightmaps opt-in.
  if (std::getenv("BF2_USE_LIGHTMAP") != nullptr) {
    const auto light_tiles = load_tile_grid(resources, cfg.lightmap_folder, rows, cols);
    if (!light_tiles.empty() && stitch_tiles(light_tiles, rows, cols, out.lightmap)) {
      out.has_lightmap = true;
    }
  }

  // Per-patch blend masks: Detailmaps/txRRxCC_1.dds (+ _2). Green blobs on a
  // white base select where the layer detail textures show. Missing tiles fill
  // black => that patch shows only the base detail.
  {
    const auto m1 = load_tile_grid(resources, cfg.detail_folder, rows, cols, "_1");
    if (!m1.empty() && stitch_tiles(m1, rows, cols, out.mask1, /*mean_fill=*/false)) {
      out.has_mask1 = true;
    }
    const auto m2 = load_tile_grid(resources, cfg.detail_folder, rows, cols, "_2");
    if (!m2.empty() && stitch_tiles(m2, rows, cols, out.mask2, /*mean_fill=*/false)) {
      out.has_mask2 = true;
    }
  }

  // Diagnostic: BF2_NO_SPLAT renders colormap-only (no detail textures) so tile
  // seams can be attributed to the colormap vs the detail splat.
  if (std::getenv("BF2_NO_SPLAT") != nullptr) {
    out.tile_cols = cols;
    out.tile_rows = rows;
    return out;
  }

  // Shared tiling detail ground textures (BF2 ships these in Common_client.zip).
  // Base = dirt/sand, layer1 = grass/vegetation, layer2 = rock. We try a small
  // candidate list so maps without a bespoke texture still get a sensible look.
  const std::string base = "Terrain/Textures/Detail/";
  out.detail0 = load_first_detail(resources, {base + "Detail_Daliandirt.dds", base + "Detail_Sandy.dds",
                                              base + "Detail_Dirt01.dds", base + "Detail_Dirt02.dds"});
  out.detail1 = load_first_detail(resources, {base + "Detail_Grass02.dds", base + "Detail_Grass01.dds",
                                              base + "Detail_Grassy1.dds", base + "Detail_Grass03.dds"});
  out.detail2 = load_first_detail(resources, {base + "Detail_Rock06.dds", base + "Detail_Rock02.dds",
                                              base + "Detail_Rocky.dds", base + "Detail_Gravel.dds"});
  out.has_detail0 = out.detail0.width > 0 && !out.detail0.pixels.empty();
  out.has_detail1 = out.detail1.width > 0 && !out.detail1.pixels.empty();
  out.has_detail2 = out.detail2.width > 0 && !out.detail2.pixels.empty();

  out.tile_cols = cols;
  out.tile_rows = rows;
  return out;
}

TerrainGroundTextures TerrainColormapLoader::upload(const TerrainGroundAtlases& atlases) {
  TerrainGroundTextures out;
  if (atlases.colormap.pixels.empty()) {
    return out;
  }
  out.colormap = upload_rgba_texture(atlases.colormap);
  if (out.colormap == 0) {
    return out;
  }
  if (atlases.has_lightmap) {
    out.lightmap = upload_rgba_texture(atlases.lightmap);
  }
  if (atlases.has_mask1) {
    out.mask1 = upload_rgba_texture(atlases.mask1);
  }
  if (atlases.has_mask2) {
    out.mask2 = upload_rgba_texture(atlases.mask2);
  }
  if (atlases.has_detail0) {
    out.detail0 = upload_rgba_texture(atlases.detail0, /*tiling=*/true);
  }
  if (atlases.has_detail1) {
    out.detail1 = upload_rgba_texture(atlases.detail1, /*tiling=*/true);
  }
  if (atlases.has_detail2) {
    out.detail2 = upload_rgba_texture(atlases.detail2, /*tiling=*/true);
  }
  // Splat is usable as long as we have the base detail; masks/layers just
  // enrich it (missing layers collapse to the base).
  out.splat = out.detail0 != 0;
  out.tile_cols = atlases.tile_cols;
  out.tile_rows = atlases.tile_rows;
  return out;
}

TerrainGroundTextures TerrainColormapLoader::load(ResourceManager& resources,
                                                  const TerrainVisualConfig& cfg) {
  const auto atlases = build_atlases(resources, cfg);
  return upload(atlases);
}

void TerrainColormapLoader::destroy(TerrainGroundTextures& textures) {
  if (textures.colormap != 0) {
    glDeleteTextures(1, &textures.colormap);
  }
  if (textures.lightmap != 0) {
    glDeleteTextures(1, &textures.lightmap);
  }
  const std::uint32_t extra[] = {textures.mask1, textures.mask2, textures.detail0, textures.detail1,
                                 textures.detail2};
  for (std::uint32_t id : extra) {
    if (id != 0) {
      glDeleteTextures(1, &id);
    }
  }
  textures = {};
}

TexturedMeshData terrain_to_textured_mesh(const Terrain& terrain, float xz_scale, int step) {
  TexturedMeshData mesh;
  const int w = static_cast<int>(terrain.width);
  const int h = static_cast<int>(terrain.height);
  const int gw = (w + step - 1) / step;
  const int gh = (h + step - 1) / step;
  const float off_x = ((w - 1.f) * 0.5f) * xz_scale;
  const float off_z = ((h - 1.f) * 0.5f) * xz_scale;
  const float u_max = static_cast<float>(w - 1);
  const float v_max = static_cast<float>(h - 1);

  auto height_at = [&](int x, int z) -> float {
    x = std::clamp(x, 0, w - 1);
    z = std::clamp(z, 0, h - 1);
    return terrain.samples[static_cast<std::size_t>(z) * w + x].height;
  };

  mesh.vertices.reserve(static_cast<std::size_t>(gw) * gh);
  for (int gz = 0; gz < gh; ++gz) {
    for (int gx = 0; gx < gw; ++gx) {
      const int x = gx * step;
      const int z = gz * step;
      ExtractedVertex v;
      v.position.x = x * xz_scale - off_x;
      v.position.y = height_at(x, z);
      v.position.z = z * xz_scale - off_z;
      v.uv[0] = static_cast<float>(x) / u_max;
      v.uv[1] = static_cast<float>(z) / v_max;
      const float hl = height_at(x - step, z);
      const float hr = height_at(x + step, z);
      const float hd = height_at(x, z - step);
      const float hu = height_at(x, z + step);
      glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * step * xz_scale, hd - hu));
      v.normal = {n.x, n.y, n.z};
      mesh.vertices.push_back(v);
    }
  }

  mesh.indices.reserve(static_cast<std::size_t>(gw - 1) * (gh - 1) * 6);
  for (int gz = 0; gz < gh - 1; ++gz) {
    for (int gx = 0; gx < gw - 1; ++gx) {
      const std::uint32_t a = gz * gw + gx;
      const std::uint32_t b = a + 1;
      const std::uint32_t c = a + gw;
      const std::uint32_t d = c + 1;
      mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
    }
  }

  bf2::TexturedSubmesh sub;
  sub.index_offset = 0;
  sub.index_count = static_cast<std::uint32_t>(mesh.indices.size());
  mesh.submeshes.push_back(sub);
  return mesh;
}

}  // namespace bf2
