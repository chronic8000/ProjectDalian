#include <iostream>
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

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: assetprobe <archive.zip> <virtual/path> [more paths...]\n";
    return 1;
  }

  bf2::ArchiveMount archive;
  if (!archive.mount(argv[1])) {
    std::cerr << "Failed to mount archive: " << argv[1] << '\n';
    return 1;
  }
  std::cout << "Mounted " << argv[1] << " (" << archive.list().size() << " files)\n\n";

  int rc = 0;
  for (int i = 2; i < argc; ++i) {
    rc |= probe(archive, argv[i]);
  }
  return rc;
}
