#include "asset_audit.hpp"

#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/render/texture_resolve.hpp"

#include <iostream>
#include <unordered_set>

namespace bf2 {

AssetAuditReport audit_static_assets(ResourceManager& resources,
                                     const TemplateResolver& resolver,
                                     const std::vector<std::string>& placement_templates) {
  AssetAuditReport report;
  report.placement_count = placement_templates.size();

  std::unordered_set<std::string> unresolved;
  std::unordered_set<std::string> mesh_keys;
  for (const auto& tmpl : placement_templates) {
    const std::string mesh = resolver.resolve_mesh(tmpl);
    if (mesh.empty()) {
      unresolved.insert(tmpl);
      continue;
    }
    mesh_keys.insert(mesh);
  }
  report.unresolved_templates = unresolved.size();
  report.unresolved_names.assign(unresolved.begin(), unresolved.end());
  report.unique_meshes = mesh_keys.size();

  auto check_slot = [&](const std::string& tmpl, const std::string& mesh_vpath,
                        const std::string& slot, const std::string& raw_path) {
    if (raw_path.empty()) return;
    const std::string folder = mesh_vpath.substr(0, mesh_vpath.find_last_of('/'));
    if (resolve_texture_vpath(resources, raw_path, folder)) return;
    TextureAuditMiss miss;
    miss.template_name = tmpl;
    miss.mesh_vpath = mesh_vpath;
    miss.slot = slot;
    miss.raw_path = raw_path;
    miss.tried = texture_candidate_paths(raw_path, folder);
    report.misses.push_back(std::move(miss));
    ++report.texture_misses;
  };

  for (const auto& mesh_vpath : mesh_keys) {
    std::string tmpl;
    for (const auto& [name, path] : resolver.map()) {
      if (path == mesh_vpath) {
        tmpl = name;
        break;
      }
    }
    try {
      const auto mesh = resources.load_mesh(mesh_vpath);
      const auto data = MeshLoader::extract_textured(mesh, 0, 0);
      for (std::size_t i = 0; i < data.submeshes.size(); ++i) {
        const auto& sub = data.submeshes[i];
        const std::string prefix = "sub" + std::to_string(i) + "/";
        check_slot(tmpl, mesh_vpath, prefix + "base", sub.base_map);
        check_slot(tmpl, mesh_vpath, prefix + "detail", sub.detail_map);
        check_slot(tmpl, mesh_vpath, prefix + "normal", sub.normal_map);
        check_slot(tmpl, mesh_vpath, prefix + "dirt", sub.dirt_map);
        check_slot(tmpl, mesh_vpath, prefix + "crack", sub.crack_map);
      }
    } catch (...) {
    }
  }
  return report;
}

void log_asset_audit(const AssetAuditReport& report, bool verbose) {
  if (report.unresolved_templates > 0) {
    std::cout << "Asset audit: " << report.unresolved_templates
              << " placements use unresolved templates\n";
    if (verbose) {
      for (const auto& n : report.unresolved_names) std::cout << "  UNRESOLVED " << n << '\n';
    }
  }
  if (report.texture_misses > 0) {
    std::cout << "Asset audit: " << report.texture_misses << " missing texture paths across "
              << report.unique_meshes << " meshes";
    if (!verbose) std::cout << " (set BF2_TEXAUDIT=1 for details)";
    std::cout << '\n';
    if (verbose) {
      for (const auto& m : report.misses) {
        std::cout << "  MISS " << m.template_name << " " << m.mesh_vpath << " " << m.slot << " raw="
                  << m.raw_path << '\n';
        for (const auto& t : m.tried) std::cout << "    tried: " << t << '\n';
      }
    }
  } else if (report.unresolved_templates == 0) {
    std::cout << "Asset audit: all " << report.unique_meshes << " static meshes resolved textures OK\n";
  }
}

}  // namespace bf2
