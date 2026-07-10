#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#include <glm/glm.hpp>

#include "engine/anim/pose.hpp"
#include "engine/anim/skinning.hpp"
#include "engine/formats/animation/bf2_animation.hpp"
#include "engine/formats/archive/archive.hpp"
#include "engine/formats/collision/bf2_collision.hpp"
#include "engine/formats/dds/dds_loader.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/core/resource_manager.hpp"
#include "engine/core/template_resolver.hpp"
#include "engine/core/static_object_parser.hpp"
#include "engine/core/overgrowth_parser.hpp"
#include "engine/core/overgrowth_instances.hpp"
#include "engine/core/compiled_roads_parser.hpp"
#include "engine/core/collision_resolver.hpp"
#include "engine/formats/mesh/bf2_road_mesh.hpp"
#include "engine/formats/terrain/heightmap_cluster.hpp"
#include "engine/formats/terrain/terrain_con_parser.hpp"
#include "engine/formats/terrain/terrain_loader.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/script/con_interpreter.hpp"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  } else {
    std::cout << "ok: " << what << '\n';
  }
}

}  // namespace

int main() {
  // --- Deterministic, install-independent checks ---
  const auto mesh = bf2::MeshLoader::load_from_file("tests/fixtures/cube.staticmesh");
  const auto extracted = bf2::MeshLoader::extract_geometry(mesh);
  check(!extracted.vertices.empty(), "cube fixture extracts vertices");

  std::vector<std::uint8_t> dds(128, 0);
  dds[0] = 'D'; dds[1] = 'D'; dds[2] = 'S'; dds[3] = ' ';
  (void)bf2::DdsLoader::load_from_memory(dds);

  std::vector<std::uint8_t> hm(4, 0);
  hm[1] = 1;
  (void)bf2::TerrainLoader::load_raw_heightmap(hm, 1, 1, 1.f);

  bf2::ConInterpreter interpreter;
  interpreter.execute_script(
      "ObjectTemplate.create SimpleObject TestObject\n"
      "ObjectTemplate.geometry test/mesh\n"
      "rem this is a comment\n"
      "ObjectTemplate.activeSafe SimpleObject TestObject\n"
      "ObjectTemplate.hasCollisionPhysics 1\n");
  check(interpreter.templates().contains("TestObject"), "con .create + .activeSafe register template");

  // Level placement parsing.
  bf2::ConInterpreter placer;
  placer.execute_script(
      "Object.create house\n"
      "Object.absolutePosition 10/20/30\n"
      "Object.rotation 90/0/0\n"
      "Object.create tree\n"
      "Object.absolutePosition -5/1.5/2\n");
  check(placer.instances().size() == 2, "Object.create parses two placements");
  check(placer.instances().size() == 2 && placer.instances()[0].position[1] == 20.f,
        "Object.absolutePosition parses y");

  check(bf2::static_object_parser_self_test(), "StaticObjectParser self-test");
  check(bf2::terrain_con_parser_self_test(), "TerrainConParser self-test");
  check(bf2::overgrowth_parser_self_test(), "OvergrowthParser self-test");
  check(bf2::overgrowth_instances_self_test(), "OvergrowthInstances self-test");
  check(bf2::compiled_roads_parser_self_test(), "CompiledRoadsParser self-test");
  check(bf2::road_compiled_self_test(), "RoadCompiled mesh self-test");
  check(bf2::collision_resolver_self_test(), "CollisionResolver self-test");
  check(bf2::MeshLoader::technique_map_assign_self_test(), "Technique map assign self-test");
  check(bf2::heightmap_cluster_self_test(), "HeightmapCluster self-test");

  // Character controller clamps to a flat terrain surface.
  {
    bf2::Terrain flat;
    flat.width = 4;
    flat.height = 4;
    flat.samples.assign(16, bf2::TerrainSample{50.f});
    bf2::PhysicsWorld world;
    world.set_terrain(flat, 10.f, /*centered=*/true);
    check(std::abs(world.terrain_height(0.f, 0.f) - 50.f) < 1e-3f, "bilinear terrain height");
    bf2::CharacterController ch;
    ch.position = {0.f, 500.f, 0.f};
    ch.eye_height = 1.8f;
    for (int i = 0; i < 300; ++i) {
      world.step_character(ch, 1.f / 30.f);
    }
    check(ch.on_ground && std::abs(ch.position.y - (50.f + 1.8f)) < 0.5f,
          "character falls and clamps to terrain surface");
  }

  // --- Real-asset regression checks (only when BF2 is installed) ---
  const std::string archive_path =
      "c:/Program Files (x86)/Battlefield2/mods/bf2/Objects_client.zip";
  if (std::filesystem::exists(archive_path)) {
    bf2::ArchiveMount archive;
    check(archive.mount(archive_path), "mount real Objects_client.zip");

    const auto flag = archive.read("Common/Flags/flag_ch/meshes/flag_ch.skinnedmesh");
    check(flag.has_value(), "read + inflate real skinnedmesh");
    if (flag) {
      const auto m = bf2::MeshLoader::load_from_memory(*flag, bf2::MeshKind::Skinned);
      const auto ex = bf2::MeshLoader::extract_geometry(m, 0, 0);
      check(ex.vertices.size() == 154 && ex.indices.size() == 720,
            "skinnedmesh extraction matches expected counts");
    }

    const auto ske = archive.read("soldiers/Common/Animations/1p_setup.ske");
    bf2::Skeleton soldier;
    if (ske) {
      soldier = bf2::SkeletonLoader::load_from_memory(*ske);
      check(soldier.version == 2 && soldier.nodes.size() == 70, "soldier .ske parses 70 bones");
      check(soldier.nodes.size() > 2 && soldier.nodes[2].parent == 1,
            "ske bone hierarchy parent links");

      // Bind-pose evaluation should place the wrist bone at a finite spot.
      const auto posed = bf2::pose_skeleton(soldier, nullptr, 0);
      const auto& wrist = posed.world_positions[7];
      check(std::isfinite(wrist.x) && std::isfinite(wrist.y) && std::isfinite(wrist.z) &&
                wrist.y > 0.5f && wrist.y < 3.0f,
            "pose_skeleton produces finite bind-pose bone position");
    }

    const auto baf = archive.read("Weapons/Handheld/at_mine/Animations/1p/1p_at_mine_idle1.baf");
    if (baf) {
      const auto a = bf2::AnimationLoader::load_from_memory(*baf);
      check(a.version == 4 && a.bone_count == 48 && a.frame_count == 91,
            ".baf parses version/bones/frames");
      if (!soldier.nodes.empty()) {
        const auto p0 = bf2::pose_skeleton(soldier, &a, 0);
        const auto p1 = bf2::pose_skeleton(soldier, &a, 40);
        const auto& w0 = p0.world_positions[7];
        const auto& w1 = p1.world_positions[7];
        const float moved = std::abs(w0.x - w1.x) + std::abs(w0.y - w1.y) + std::abs(w0.z - w1.z);
        check(moved > 1e-4f, "animation changes posed bone positions across frames");
      }
    }

    const auto col = archive.read("Common/Flags/flagpole/meshes/flagpole.collisionmesh");
    if (col) {
      const auto c = bf2::CollisionLoader::load_from_memory(*col);
      check(!c.cols.empty(), ".collisionmesh parses cols");
    }

  const auto flag_mesh = archive.read("Common/Flags/flag_ch/meshes/flag_ch.skinnedmesh");
  const auto flag_ske = archive.read("Common/Flags/flag_setup.ske");
  const auto flag_baf = archive.read("Common/Flags/animations/flag_idle.baf");
  if (flag_mesh && flag_ske) {
    const auto mesh = bf2::MeshLoader::load_from_memory(*flag_mesh, bf2::MeshKind::Skinned);
    const auto skeleton = bf2::SkeletonLoader::load_from_memory(*flag_ske);
    const auto bind = bf2::skin_mesh(mesh, skeleton, nullptr, 0, 0, 0);
    check(!bind.vertices.empty() && bind.vertices.size() == mesh.vertex_count,
          "skin_mesh bind pose keeps vertex count");
    float max_y = 0.f;
    for (const auto& v : bind.vertices) {
      max_y = std::max(max_y, std::abs(v.position.y));
      check(std::isfinite(v.position.x) && std::isfinite(v.position.y) &&
                std::isfinite(v.position.z),
            "skin_mesh bind pose vertices are finite");
    }
    check(max_y < 5.f, "skin_mesh flag bind pose stays in a sane volume");
    if (flag_baf) {
      const auto clip = bf2::AnimationLoader::load_from_memory(*flag_baf);
      const auto posed = bf2::skin_mesh(mesh, skeleton, &clip, 30, 0, 0);
      float delta = 0.f;
      for (std::size_t i = 0; i < bind.vertices.size(); ++i) {
        delta += std::abs(bind.vertices[i].position.x - posed.vertices[i].position.x);
        delta += std::abs(bind.vertices[i].position.y - posed.vertices[i].position.y);
        delta += std::abs(bind.vertices[i].position.z - posed.vertices[i].position.z);
      }
      check(delta > 1e-3f, "skin_mesh flag animation deforms vertices");
    }
  }

  const auto sol_mesh =
      archive.read("soldiers/Us/Meshes/us_light_soldier.skinnedmesh");
  const auto sol_ske = archive.read("soldiers/Common/Animations/3p_setup.ske");
  const auto sol_baf = archive.read("soldiers/Common/Animations/3P/3p_runForward.baf");
  if (sol_mesh && sol_ske) {
    const auto mesh = bf2::MeshLoader::load_from_memory(*sol_mesh, bf2::MeshKind::Skinned);
    const auto skeleton = bf2::SkeletonLoader::load_from_memory(*sol_ske);
    const auto bind = bf2::skin_mesh(mesh, skeleton, nullptr, 0, 1, 0);
    check(bind.vertices.size() > 1000, "skin_mesh soldier 3p geometry extracts verts");
    float span = 0.f;
    for (const auto& v : bind.vertices) {
      span = std::max(span, std::abs(v.position.y));
      check(std::isfinite(v.position.x), "skin_mesh soldier bind verts finite");
    }
    check(span > 0.5f && span < 3.f, "skin_mesh soldier bind pose height is human-scale");
    if (sol_baf) {
      const auto clip = bf2::AnimationLoader::load_from_memory(*sol_baf);
      const auto posed = bf2::skin_mesh(mesh, skeleton, &clip, 4, 1, 0);
      float delta = 0.f;
      for (std::size_t i = 0; i < bind.vertices.size(); ++i) {
        delta += std::abs(bind.vertices[i].position.x - posed.vertices[i].position.x);
        delta += std::abs(bind.vertices[i].position.y - posed.vertices[i].position.y);
      }
      check(delta > 1e-2f, "skin_mesh soldier run animation deforms vertices");

      // Template resolution: follow a real level's StaticObjects.con run-lines
      // and map placement templates to concrete static meshes.
      const std::string lvl =
          "c:/Program Files (x86)/Battlefield2/mods/bf2/Levels/Dalian_plant/server.zip";
      if (std::filesystem::exists(lvl)) {
        bf2::ResourceManager res;
        res.archives().mount(lvl);
        res.archives().mount(archive_path);
        if (const auto so = res.read_bytes("StaticObjects.con")) {
          bf2::TemplateResolver resolver(res);
          const std::string script(reinterpret_cast<const char*>(so->data()), so->size());
          resolver.build_from_static_objects(script);
          check(resolver.map().size() > 50, "resolver maps many templates to meshes");
          const std::string mesh_vpath = resolver.resolve_mesh("lrgfactorybuilding");
          check(!mesh_vpath.empty() && res.archives().exists(mesh_vpath),
                "resolver finds lrgfactorybuilding static mesh");
        }
      }

      // GPU skinning parity: applying the shader math (palette[bone]*bind_pos)
      // on the static geometry must reproduce the CPU-skinned positions.
      const auto geom = bf2::extract_skinned(mesh, skeleton, 1, 0);
      const auto palette = bf2::compute_skin_palette(mesh, skeleton, &clip, 4, 1, 0);
      float max_err = 0.f;
      for (const std::uint32_t idx : geom.indices) {  // only rendered vertices
        const auto& sv = geom.vertices[idx];
        const glm::vec4 p(sv.position.x, sv.position.y, sv.position.z, 1.0f);
        const glm::mat4 m = palette[sv.bone[0]] * sv.weight + palette[sv.bone[1]] * (1.0f - sv.weight);
        const glm::vec3 gpu = glm::vec3(m * p);
        const glm::vec3 cpu(posed.vertices[idx].position.x, posed.vertices[idx].position.y,
                            posed.vertices[idx].position.z);
        max_err = std::max(max_err, glm::length(gpu - cpu));
      }
      check(max_err < 1e-3f, "GPU skin palette matches CPU skin_mesh output");
    }
  }
  } else {
    std::cout << "skip: BF2 not installed, real-asset checks bypassed\n";
  }

  if (failures == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
