#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/archive/archive.hpp"
#include "engine/formats/collision/bf2_collision.hpp"
#include "engine/formats/dds/dds_loader.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/script/con_interpreter.hpp"

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bf2::MeshKind mesh_kind(const std::string& path) {
  const auto l = lower(path);
  if (l.ends_with("bundledmesh")) return bf2::MeshKind::Bundled;
  if (l.ends_with("skinnedmesh")) return bf2::MeshKind::Skinned;
  return bf2::MeshKind::Static;
}

int probe(bf2::ArchiveMount& archive, const std::string& vpath) {
  const auto bytes = archive.read(vpath);
  if (!bytes) {
    std::cerr << "  NOT FOUND: " << vpath << '\n';
    return 1;
  }
  const auto l = lower(vpath);
  std::cout << "[" << vpath << "] (" << bytes->size() << " bytes)\n";
  try {
    if (l.ends_with("mesh") && !l.ends_with("collisionmesh")) {
      const auto mesh = bf2::MeshLoader::load_from_memory(*bytes, mesh_kind(vpath));
      const auto info = bf2::MeshLoader::summarize(mesh);
      std::cout << "  mesh v" << info.version << " geoms=" << info.geometry_count
                << " lods=" << info.lod_count << " mats=" << info.material_count
                << " verts=" << info.vertex_count << " indices=" << info.index_count << '\n';
    } else if (l.ends_with("collisionmesh")) {
      const auto col = bf2::CollisionLoader::load_from_memory(*bytes);
      std::size_t verts = 0, faces = 0;
      for (const auto& lod : col.lods) {
        verts += lod.vertices.size();
        faces += lod.faces.size();
      }
      std::cout << "  collision lods=" << col.lods.size() << " verts=" << verts
                << " faces=" << faces << '\n';
    } else if (l.ends_with(".ske")) {
      const auto ske = bf2::SkeletonLoader::load_from_memory(*bytes);
      std::cout << "  skeleton v" << ske.version << " nodes=" << ske.nodes.size() << '\n';
      for (std::size_t i = 0; i < ske.nodes.size() && i < 6; ++i) {
        std::cout << "    bone[" << i << "] " << ske.nodes[i].name
                  << " parent=" << ske.nodes[i].parent << '\n';
      }
    } else if (l.ends_with(".baf")) {
      const auto clip = bf2::AnimationLoader::load_from_memory(*bytes);
      std::cout << "  anim v" << clip.version << " bones=" << clip.bone_count
                << " frames=" << clip.frame_count << " tracks=" << clip.tracks.size() << '\n';
    } else if (l.ends_with(".dds")) {
      const auto tex = bf2::DdsLoader::load_from_memory(*bytes);
      const char* fmt = "?";
      switch (tex.format) {
        case bf2::DdsFormat::DXT1: fmt = "DXT1"; break;
        case bf2::DdsFormat::DXT3: fmt = "DXT3"; break;
        case bf2::DdsFormat::DXT5: fmt = "DXT5"; break;
        case bf2::DdsFormat::RGBA8: fmt = "RGBA8"; break;
        default: fmt = "unknown"; break;
      }
      std::cout << "  dds " << tex.width << "x" << tex.height << " fmt=" << fmt
                << " payload=" << tex.pixels.size() << '\n';
    } else if (l.ends_with(".con") || l.ends_with(".tweak")) {
      std::string script(reinterpret_cast<const char*>(bytes->data()), bytes->size());
      bf2::ConInterpreter interp;
      interp.execute_script(script);
      std::cout << "  con templates=" << interp.templates().size()
                << " placements=" << interp.instances().size() << '\n';
      for (std::size_t i = 0; i < interp.instances().size() && i < 3; ++i) {
        const auto& inst = interp.instances()[i];
        std::cout << "    place " << inst.template_name << " @ " << inst.position[0] << "/"
                  << inst.position[1] << "/" << inst.position[2] << '\n';
      }
      std::size_t shown = 0;
      for (const auto& [name, tmpl] : interp.templates()) {
        if (shown++ >= 4) break;
        std::cout << "    template " << name << " props=" << tmpl.properties.size()
                  << " children=" << tmpl.children.size() << '\n';
      }
    } else {
      std::cout << "  (no loader for this extension)\n";
    }
  } catch (const std::exception& ex) {
    std::cerr << "  ERROR: " << ex.what() << '\n';
    return 2;
  }
  return 0;
}

}  // namespace

void dump_mesh_detail(bf2::ArchiveMount& archive, const std::string& vpath) {
  const auto bytes = archive.read(vpath);
  if (!bytes) {
    std::cerr << "  NOT FOUND: " << vpath << '\n';
    return;
  }
  const auto mesh = bf2::MeshLoader::load_from_memory(*bytes, mesh_kind(vpath));
  std::cout << "[" << vpath << "] geoms=" << mesh.geometries.size()
            << " stride=" << mesh.vertex_stride << " verts=" << mesh.vertex_count << '\n';
  for (std::size_t i = 0; i < mesh.vertex_attributes.size(); ++i) {
    const auto& a = mesh.vertex_attributes[i];
    std::cout << "  attr[" << i << "] flag=" << a.flag << " off=" << a.offset
              << " type=" << a.vartype << " usage=" << a.usage << '\n';
  }
  // Per-bone (per geometry-part) vertex bounds: reveals whether bundledmesh parts
  // are stored in model space (assembled) or per-part local space (need transforms).
  if (mesh_kind(vpath) == bf2::MeshKind::Bundled && mesh.vertex_stride >= 28 &&
      !mesh.vertex_data.empty()) {
    const std::size_t fstride = mesh.vertex_stride / 4;
    const std::size_t nverts = mesh.vertex_data.size() / fstride;
    struct BB {
      float mn[3] = {1e30f, 1e30f, 1e30f};
      float mx[3] = {-1e30f, -1e30f, -1e30f};
      std::size_t n = 0;
    };
    std::map<int, BB> bb;
    for (std::size_t v = 0; v < nverts; ++v) {
      const float* p = &mesh.vertex_data[v * fstride];
      // bone index = first byte of the UBYTE4 at byte offset 24 (float index 6)
      float raw = mesh.vertex_data[v * fstride + 6];
      unsigned char bytes[4];
      std::memcpy(bytes, &raw, 4);
      int bone = bytes[0];
      auto& b = bb[bone];
      for (int k = 0; k < 3; ++k) {
        b.mn[k] = std::min(b.mn[k], p[k]);
        b.mx[k] = std::max(b.mx[k], p[k]);
      }
      ++b.n;
    }
    std::cout << " per-bone bounds (bundledmesh):\n";
    for (const auto& [bone, b] : bb) {
      std::cout << "   bone[" << bone << "] n=" << b.n << " min(" << b.mn[0] << "," << b.mn[1]
                << "," << b.mn[2] << ") max(" << b.mx[0] << "," << b.mx[1] << "," << b.mx[2]
                << ")\n";
    }
  }
  // For each geometry/lod, report the AABB and the distinct bone indices its
  // triangles actually reference (via the shared index/vertex buffers).
  if (mesh_kind(vpath) == bf2::MeshKind::Bundled && mesh.vertex_stride >= 28 &&
      !mesh.vertex_data.empty() && !mesh.indices.empty()) {
    const std::size_t fstride = mesh.vertex_stride / 4;
    for (std::size_t g = 0; g < mesh.geometries.size(); ++g) {
      for (std::size_t l = 0; l < mesh.geometries[g].lods.size(); ++l) {
        const auto& lod = mesh.geometries[g].lods[l];
        std::set<int> bones;
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (const auto& mat : lod.materials) {
          for (std::uint32_t i = 0; i < mat.index_count; ++i) {
            const std::uint32_t vi =
                mat.vertex_start + mesh.indices[mat.index_start + i];
            if (vi >= mesh.vertex_count) continue;
            const float* p = &mesh.vertex_data[vi * fstride];
            float raw = mesh.vertex_data[vi * fstride + 6];
            unsigned char b4[4];
            std::memcpy(b4, &raw, 4);
            bones.insert(b4[0]);
            for (int k = 0; k < 3; ++k) {
              mn[k] = std::min(mn[k], p[k]);
              mx[k] = std::max(mx[k], p[k]);
            }
          }
        }
        std::cout << " ref geom[" << g << "].lod[" << l << "] bones={";
        for (int b : bones) std::cout << b << ",";
        std::cout << "} aabb Y[" << mn[1] << "," << mx[1] << "] Z[" << mn[2] << "," << mx[2]
                  << "]\n";
      }
    }
  }
  for (std::size_t g = 0; g < mesh.geometries.size(); ++g) {
    const auto& geom = mesh.geometries[g];
    std::cout << " geom[" << g << "] lods=" << geom.lods.size() << '\n';
    for (std::size_t l = 0; l < geom.lods.size(); ++l) {
      const auto& lod = geom.lods[l];
      std::cout << "   lod[" << l << "] mats=" << lod.materials.size()
                << " rigs=" << lod.rigs.size() << " nodes=" << lod.nodes.size() << '\n';
      for (std::size_t m = 0; m < lod.materials.size(); ++m) {
        const auto& mat = lod.materials[m];
        std::cout << "     mat[" << m << "] verts=" << mat.vertex_count
                  << " idx=" << mat.index_count << " tech=" << mat.technique;
        if (!mat.maps.empty()) {
          std::cout << " maps=";
          for (std::size_t mi = 0; mi < mat.maps.size(); ++mi) {
            if (mi) std::cout << "|";
            std::cout << mat.maps[mi];
          }
        }
        std::cout << '\n';
      }
    }
  }
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: assetprobe <archive.zip> <virtual/path> [more paths...]\n"
                 "       assetprobe <archive.zip> --list [prefix]\n"
                 "       assetprobe <archive.zip> --dump <mesh/path>\n";
    return 1;
  }

  bf2::ArchiveMount archive;
  if (!archive.mount(argv[1])) {
    std::cerr << "Failed to mount archive: " << argv[1] << '\n';
    return 1;
  }
  std::cout << "Mounted " << argv[1] << " (" << archive.list().size() << " files)\n\n";

  if (std::string(argv[2]) == "--list") {
    const std::string prefix = argc >= 4 ? lower(argv[3]) : std::string();
    for (const auto& p : archive.list()) {
      if (prefix.empty() || lower(p).find(prefix) != std::string::npos) std::cout << p << '\n';
    }
    return 0;
  }
  if (std::string(argv[2]) == "--dump") {
    for (int i = 3; i < argc; ++i) dump_mesh_detail(archive, argv[i]);
    return 0;
  }
  if (std::string(argv[2]) == "--cat") {
    for (int i = 3; i < argc; ++i) {
      const auto bytes = archive.read(argv[i]);
      if (!bytes) {
        std::cerr << "NOT FOUND: " << argv[i] << '\n';
        continue;
      }
      std::cout << "===== " << argv[i] << " =====\n";
      std::cout.write(reinterpret_cast<const char*>(bytes->data()),
                      static_cast<std::streamsize>(bytes->size()));
      std::cout << "\n";
    }
    return 0;
  }

  int rc = 0;
  for (int i = 2; i < argc; ++i) {
    rc |= probe(archive, argv[i]);
  }
  return rc;
}
