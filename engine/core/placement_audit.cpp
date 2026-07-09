#include "placement_audit.hpp"

#include "engine/core/mesh_bounds.hpp"
#include "engine/core/overgrowth_instances.hpp"
#include "engine/core/overgrowth_parser.hpp"
#include "engine/formats/terrain/terrain_colormap.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace bf2 {
namespace {

bool mount_level_archives(ResourceManager& resources, const std::string& mod_dir,
                          const std::string& server_zip, const std::string& bf2_root) {
  resources.archives().clear();
  if (!resources.archives().mount(server_zip)) return false;
  const fs::path level_dir = fs::path(server_zip).parent_path();
  const fs::path client_zip = level_dir / "client.zip";
  if (fs::exists(client_zip)) resources.archives().mount(client_zip.string());
  auto mount_mod_zips = [&](const fs::path& mdir) {
    for (const char* name :
         {"Objects_client.zip", "Objects_server.zip", "Common_client.zip", "Common_server.zip",
          "Booster_client.zip", "Booster_server.zip"}) {
      const fs::path p = mdir / name;
      if (fs::exists(p)) resources.archives().mount(p.string());
    }
  };
  mount_mod_zips(fs::path(mod_dir));
  const std::string mod_name = fs::path(mod_dir).filename().string();
  if (!bf2_root.empty() && mod_name != "bf2") {
    const fs::path bf2_mod = fs::path(bf2_root) / "mods" / "bf2";
    if (fs::exists(bf2_mod)) mount_mod_zips(bf2_mod);
  }
  return true;
}

void add_entry(PlacementAuditReport& rep, PlacementAuditEntry e, std::size_t max_outliers) {
  ++rep.total;
  if (e.gap > rep.float_threshold) ++rep.float_count;
  if (e.gap < -rep.embed_threshold) ++rep.embed_count;
  if ((e.gap > rep.float_threshold || e.gap < -rep.embed_threshold) &&
      rep.outliers.size() < max_outliers) {
    rep.outliers.push_back(std::move(e));
  }
}

}  // namespace

PlacementAuditReport audit_placement_heights(const std::string& bf2_root, const std::string& mod,
                                             const std::string& level_name, float float_threshold,
                                             float embed_threshold, std::size_t max_outliers) {
  PlacementAuditReport rep;
  rep.mod = mod;
  rep.level = level_name;
  rep.float_threshold = float_threshold;
  rep.embed_threshold = embed_threshold;

  const fs::path server = fs::path(bf2_root) / "mods" / mod / "Levels" / level_name / "server.zip";
  if (!fs::exists(server)) return rep;

  ResourceManager resources;
  const std::string mod_dir = (fs::path(bf2_root) / "mods" / mod).string();
  if (!mount_level_archives(resources, mod_dir, server.string(), bf2_root)) return rep;

  LevelLoader loader(resources);
  const auto level = loader.load_mounted_level(server.string());
  if (!level.has_terrain) return rep;

  float xz = 2.f;
  std::string heightdata_con;
  if (const auto hd = resources.read_bytes("Heightdata.con")) {
    heightdata_con.assign(reinterpret_cast<const char*>(hd->data()), hd->size());
    xz = parse_heightmap_xz_scale(heightdata_con, xz);
  }

  PhysicsWorld world;
  world.set_terrain(level.terrain, xz, true);
  if (level.has_heightmap_cluster) world.set_heightmap_cluster(&level.heightmap_cluster);

  TemplateResolver resolver(resources);
  std::string static_script, dummy_script;
  if (const auto so = resources.read_bytes("StaticObjects.con")) {
    static_script.assign(reinterpret_cast<const char*>(so->data()), so->size());
  }
  if (const auto dummy = resources.read_bytes("DummyObjects.con")) {
    dummy_script.assign(reinterpret_cast<const char*>(dummy->data()), dummy->size());
  }
  resolver.build_from_level_scripts(static_script, dummy_script);

  std::unordered_map<std::string, float> min_y_cache;

  for (const auto& inst : level.placements) {
    const std::string vpath = resolver.resolve_mesh(inst.template_name);
    if (vpath.empty()) continue;
    const float min_y = mesh_local_min_y(resources, vpath, &min_y_cache);
    const float terr = world.terrain_height(inst.position[0], inst.position[2]);
    const float foot = inst.position[1] + min_y;
    PlacementAuditEntry e;
    e.category = "static";
    e.name = inst.template_name;
    e.x = inst.position[0];
    e.z = inst.position[2];
    e.placed_y = inst.position[1];
    e.terrain_y = terr;
    e.mesh_min_y = min_y;
    e.mesh_foot_y = foot;
    e.gap = foot - terr;
    add_entry(rep, e, max_outliers);
  }

  OvergrowthParser og_defs;
  if (const auto og = resources.read_bytes("Overgrowth/Overgrowth.con")) {
    og_defs.parse(std::string(reinterpret_cast<const char*>(og->data()), og->size()));
  }
  if (!og_defs.types().empty()) {
    if (const auto raw = resources.read_bytes("Overgrowth/Overgrowth.raw")) {
      const auto trees = build_overgrowth_instances(og_defs, *raw, resources, xz);
      for (const auto& tr : trees) {
        const float min_y = mesh_local_min_y(resources, tr.mesh_vpath, &min_y_cache);
        const float terr = world.terrain_height(tr.position[0], tr.position[2]);
        const float origin_y = terr - min_y * tr.scale;
        const float foot = origin_y + min_y * tr.scale;
        PlacementAuditEntry e;
        e.category = "overgrowth";
        e.name = tr.mesh_vpath;
        e.x = tr.position[0];
        e.z = tr.position[2];
        e.placed_y = origin_y;
        e.terrain_y = terr;
        e.mesh_min_y = min_y;
        e.mesh_foot_y = foot;
        e.gap = foot - terr;
        add_entry(rep, e, max_outliers);
      }
    }
  }

  return rep;
}

void log_placement_audit(const PlacementAuditReport& report, bool verbose) {
  std::cout << report.mod << '/' << report.level << " placement audit: total=" << report.total
            << " float>" << report.float_threshold << '=' << report.float_count << " embed<"
            << -report.embed_threshold << '=' << report.embed_count << '\n';
  if (!verbose) return;
  for (const auto& e : report.outliers) {
    std::cout << "  [" << e.category << "] " << e.name << " @ " << e.x << ',' << e.z
              << " foot=" << e.mesh_foot_y << " terr=" << e.terrain_y << " gap=" << e.gap << '\n';
  }
}

}  // namespace bf2
