#include "level_validator.hpp"

#include "engine/formats/archive/archive.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace bf2 {
namespace {

bool mount_level_archives(ResourceManager& resources, const std::string& mod_dir,
                          const std::string& server_zip, const std::string& bf2_root = {}) {
  resources.archives().clear();
  if (!resources.archives().mount(server_zip)) return false;

  const fs::path mod_path(mod_dir);
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
  mount_mod_zips(mod_path);
  // Custom mods: fall back to retail bf2 archives (incl. Booster / xp2 props).
  const std::string mod_name = mod_path.filename().string();
  if (!bf2_root.empty() && mod_name != "bf2") {
    const fs::path bf2_mod = fs::path(bf2_root) / "mods" / "bf2";
    if (fs::exists(bf2_mod)) mount_mod_zips(bf2_mod);
  }
  // SF / xpack maps chain bf2 + xpack object archives (xp1 props, shared meshes).
  if (!bf2_root.empty() && (mod_name == "bf2sf64" || mod_name == "xpack")) {
    const fs::path xpack_mod = fs::path(bf2_root) / "mods" / "xpack";
    if (fs::exists(xpack_mod)) {
      for (const char* name :
           {"Objects_client.zip", "Objects_server.zip", "Common_client.zip", "Common_server.zip"}) {
        const fs::path p = xpack_mod / name;
        if (fs::exists(p)) resources.archives().mount(p.string());
      }
    }
  }
  return true;
}

}  // namespace

std::vector<ModLevelEntry> discover_levels(const std::string& bf2_root) {
  std::vector<ModLevelEntry> out;
  const fs::path mods = fs::path(bf2_root) / "mods";
  if (!fs::is_directory(mods)) return out;
  for (const auto& mod_entry : fs::directory_iterator(mods)) {
    if (!mod_entry.is_directory()) continue;
    const fs::path levels = mod_entry.path() / "Levels";
    if (!fs::is_directory(levels)) continue;
    for (const auto& level_entry : fs::directory_iterator(levels)) {
      if (!level_entry.is_directory()) continue;
      const fs::path server = level_entry.path() / "server.zip";
      if (!fs::exists(server)) continue;
      out.push_back(
          {mod_entry.path().filename().string(), level_entry.path().filename().string(),
           server.string()});
    }
  }
  return out;
}

LevelValidationResult validate_level(const std::string& bf2_root, const std::string& mod,
                                     const std::string& level_name) {
  LevelValidationResult r;
  r.mod = mod;
  r.level = level_name;
  const fs::path server =
      fs::path(bf2_root) / "mods" / mod / "Levels" / level_name / "server.zip";
  r.server_zip = server.string();
  if (!fs::exists(server)) {
    r.error = "server.zip missing";
    return r;
  }

  ResourceManager resources;
  const fs::path mod_dir = fs::path(bf2_root) / "mods" / mod;
  if (!mount_level_archives(resources, mod_dir.string(), server.string(), bf2_root)) {
    r.error = "archive mount failed";
    return r;
  }

  LevelLoader loader(resources);
  LevelLoadResult level;
  try {
    level = loader.load_mounted_level(level_name);
  } catch (const std::exception& ex) {
    r.error = ex.what();
    return r;
  } catch (...) {
    r.error = "level load threw";
    return r;
  }

  r.load_ok = true;
  r.placements = level.placements.size();
  r.roads = level.roads.size();
  r.has_terrain = level.has_terrain || level.has_heightmap_cluster;
  r.has_nav = resources.archives().exists("GTSData/output/Infantry.vbf");
  r.has_overgrowth = resources.archives().exists("Overgrowth/Overgrowth.con");
  r.has_undergrowth = resources.archives().exists("Undergrowth.cfg");
  r.has_sky = resources.archives().exists("Sky.con");

  TemplateResolver resolver(resources);
  std::string static_script, dummy_script;
  if (const auto so = resources.read_bytes("StaticObjects.con")) {
    static_script.assign(reinterpret_cast<const char*>(so->data()), so->size());
  }
  if (const auto dummy = resources.read_bytes("DummyObjects.con")) {
    dummy_script.assign(reinterpret_cast<const char*>(dummy->data()), dummy->size());
  }
  resolver.build_from_level_scripts(static_script, dummy_script);

  std::vector<std::string> tmpl_names;
  tmpl_names.reserve(level.placements.size());
  for (const auto& p : level.placements) tmpl_names.push_back(p.template_name);
  r.audit = audit_static_assets(resources, resolver, tmpl_names);
  return r;
}

}  // namespace bf2
