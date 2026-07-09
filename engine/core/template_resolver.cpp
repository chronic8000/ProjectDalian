#include "template_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string normalize_run_path(std::string path) {
  for (auto& c : path) {
    if (c == '\\') c = '/';
  }
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  const std::string prefix = "objects/";
  if (lower(path).compare(0, prefix.size(), prefix) == 0) path.erase(0, prefix.size());
  return path;
}

std::string dir_of(const std::string& vpath) {
  const auto slash = vpath.find_last_of('/');
  return slash == std::string::npos ? std::string() : vpath.substr(0, slash);
}

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream ls(line);
  std::string tok;
  while (ls >> tok) tokens.push_back(tok);
  return tokens;
}

bool ends_with_ci(const std::string& s, const char* ext) {
  const std::string l = lower(s);
  const std::string e = ext;
  return l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0;
}

}  // namespace

TemplateResolver::TemplateResolver(ResourceManager& resources) : resources_(resources) {}

void TemplateResolver::build_from_level_scripts(const std::string& static_objects_script,
                                                const std::string& dummy_objects_script) {
  if (!static_objects_script.empty()) follow_run_script(static_objects_script);
  if (!dummy_objects_script.empty()) follow_run_script(dummy_objects_script);
}

void TemplateResolver::build_from_static_objects(const std::string& static_objects_script) {
  follow_run_script(static_objects_script);
}

void TemplateResolver::follow_run_script(const std::string& script) {
  std::istringstream in(script);
  std::string line;
  while (std::getline(in, line)) {
    const auto tokens = tokenize(line);
    if (tokens.size() < 2) continue;
    if (lower(tokens[0]) != "run") continue;
    std::string con_vpath = normalize_run_path(tokens[1]);
    const std::string lc_path = lower(con_vpath);
    const bool is_script = ends_with_ci(lc_path, ".con") || ends_with_ci(lc_path, ".tweak");
    if (!is_script) continue;
    if (std::find(visited_.begin(), visited_.end(), lc_path) != visited_.end()) continue;
    visited_.push_back(lc_path);
    if (const auto bytes = resources_.read_bytes(con_vpath)) {
      const std::string child(reinterpret_cast<const char*>(bytes->data()), bytes->size());
      parse_object_con(child, con_vpath);
    }
  }
}

std::string TemplateResolver::find_mesh_for_geometry(const std::string& folder,
                                                     const std::string& geom) const {
  if (geom.empty()) return {};
  const std::string g = lower(geom);
  const char* exts[] = {".staticmesh", ".bundledmesh", ".skinnedmesh"};
  const std::vector<std::string> bases = {
      folder + "/meshes/" + g,
      folder + "/" + g,
      "meshes/" + g,
  };
  for (const auto& base : bases) {
    for (const char* ext : exts) {
      const std::string candidate = base + ext;
      if (resources_.archives().exists(candidate)) return candidate;
    }
  }
  return find_mesh_global(g);
}

std::string TemplateResolver::find_mesh_global(const std::string& geom_lower) const {
  if (geom_lower.empty()) return {};
  ensure_mesh_index();
  const auto lookup = [&](const std::string& stem) -> std::string {
    const auto it = mesh_by_stem_.find(stem);
    return it == mesh_by_stem_.end() ? std::string() : it->second;
  };
  if (std::string hit = lookup(geom_lower); !hit.empty()) return hit;
  // Composite variants: xp2_windmill_01a -> xp2_windmill_02, hydrowires_set5 -> set1, etc.
  for (std::size_t trim = 1; trim <= 3 && geom_lower.size() > trim + 2; ++trim) {
    const char c = geom_lower[geom_lower.size() - trim];
    if (std::isalnum(static_cast<unsigned char>(c))) continue;
    if (std::string hit = lookup(geom_lower.substr(0, geom_lower.size() - trim)); !hit.empty())
      return hit;
  }
  const auto us = geom_lower.rfind('_');
  if (us != std::string::npos && us + 1 < geom_lower.size()) {
    const std::string prefix = geom_lower.substr(0, us);
    for (const auto& [stem, path] : mesh_by_stem_) {
      if (stem.rfind(prefix, 0) == 0) return path;
    }
  }
  return {};
}

void TemplateResolver::ensure_mesh_index() const {
  if (mesh_index_built_) return;
  mesh_index_built_ = true;
  const char* exts[] = {".staticmesh", ".bundledmesh", ".skinnedmesh"};
  for (const auto& path : resources_.archives().list()) {
    const std::string lp = lower(path);
    for (const char* ext : exts) {
      if (lp.size() <= std::strlen(ext) || lp.compare(lp.size() - std::strlen(ext), std::strlen(ext), ext) != 0)
        continue;
      const auto slash = lp.find_last_of('/');
      const std::string stem = slash == std::string::npos ? lp.substr(0, lp.size() - std::strlen(ext))
                                                        : lp.substr(slash + 1, lp.size() - slash - 1 - std::strlen(ext));
      if (!stem.empty() && !mesh_by_stem_.contains(stem)) mesh_by_stem_[stem] = path;
      break;
    }
  }
}

void TemplateResolver::resolve_template_defs(
    const std::unordered_map<std::string, TemplateDef>& defs) {
  for (const auto& [name, def] : defs) {
    if (template_to_mesh_.contains(name)) continue;
    std::string mesh = find_mesh_for_geometry(def.folder, def.geometry);
    if (mesh.empty()) {
      for (const auto& child : def.children) {
        const auto cit = defs.find(lower(child));
        if (cit == defs.end()) continue;
        mesh = find_mesh_for_geometry(cit->second.folder.empty() ? def.folder : cit->second.folder,
                                      cit->second.geometry.empty() ? lower(child)
                                                                   : cit->second.geometry);
        if (!mesh.empty()) break;
        for (const auto& grand : cit->second.children) {
          const auto git = defs.find(lower(grand));
          if (git == defs.end()) continue;
          mesh = find_mesh_for_geometry(
              git->second.folder.empty() ? cit->second.folder : git->second.folder,
              git->second.geometry.empty() ? lower(grand) : git->second.geometry);
          if (!mesh.empty()) break;
        }
        if (!mesh.empty()) break;
      }
    }
    if (mesh.empty() && !def.geometry.empty()) {
      mesh = find_mesh_global(lower(def.geometry));
    }
    if (mesh.empty()) mesh = find_mesh_global(name);
    if (!mesh.empty()) template_to_mesh_[name] = mesh;
  }
}

void TemplateResolver::parse_object_con(const std::string& script, const std::string& con_vpath) {
  const std::string folder = dir_of(con_vpath);
  std::unordered_map<std::string, TemplateDef> defs;
  std::string current_template;
  std::unordered_map<std::string, std::string> geom_aliases;

  auto ensure_def = [&](const std::string& name) -> TemplateDef& {
    const std::string key = lower(name);
    auto& d = defs[key];
    if (d.folder.empty()) d.folder = folder;
    return d;
  };

  std::istringstream in(script);
  std::string line;
  while (std::getline(in, line)) {
    const auto tokens = tokenize(line);
    if (tokens.empty()) continue;
    const std::string cmd = lower(tokens[0]);

    if (cmd == "geometrytemplate.create" && tokens.size() >= 3) {
      geom_aliases[lower(tokens[2])] = lower(tokens[2]);
    } else if ((cmd == "objecttemplate.create" || cmd == "objecttemplate.activesafe") &&
               tokens.size() >= 3) {
      current_template = lower(tokens.back());
      ensure_def(current_template);
    } else if (cmd == "objecttemplate.geometry" && !current_template.empty() && tokens.size() >= 2) {
      ensure_def(current_template).geometry = lower(tokens[1]);
    } else if (cmd == "objecttemplate.collisionmesh" && !current_template.empty() &&
               tokens.size() >= 2 && ensure_def(current_template).geometry.empty()) {
      ensure_def(current_template).geometry = lower(tokens[1]);
    } else if (cmd == "objecttemplate.addtemplate" && !current_template.empty() &&
               tokens.size() >= 2) {
      ensure_def(current_template).children.push_back(lower(tokens[1]));
    } else if (cmd == "include" && tokens.size() >= 2) {
      std::string inc = tokens[1];
      for (auto& c : inc) {
        if (c == '\\') c = '/';
      }
      if (inc.find('/') == std::string::npos) inc = folder + "/" + inc;
      const std::string lc_inc = lower(inc);
      if (std::find(visited_.begin(), visited_.end(), lc_inc) != visited_.end()) continue;
      visited_.push_back(lc_inc);
      if (const auto bytes = resources_.read_bytes(inc)) {
        const std::string inc_script(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        parse_object_con(inc_script, inc);
      } else if (!ends_with_ci(inc, ".con")) {
        const std::string as_con = inc + ".con";
        if (const auto bytes = resources_.read_bytes(as_con)) {
          const std::string inc_script(reinterpret_cast<const char*>(bytes->data()), bytes->size());
          const std::string lc = lower(as_con);
          if (std::find(visited_.begin(), visited_.end(), lc) == visited_.end()) {
            visited_.push_back(lc);
            parse_object_con(inc_script, as_con);
          }
        }
      }
    }
  }

  resolve_template_defs(defs);

  // Nested runs.
  std::istringstream in2(script);
  while (std::getline(in2, line)) {
    const auto tokens = tokenize(line);
    if (tokens.size() < 2 || lower(tokens[0]) != "run") continue;
    std::string child = normalize_run_path(tokens[1]);
    const std::string lc_child = lower(child);
    if (!ends_with_ci(lc_child, ".con") && !ends_with_ci(lc_child, ".tweak")) continue;
    if (std::find(visited_.begin(), visited_.end(), lc_child) != visited_.end()) continue;
    visited_.push_back(lc_child);
    if (const auto bytes = resources_.read_bytes(child)) {
      const std::string child_script(reinterpret_cast<const char*>(bytes->data()), bytes->size());
      parse_object_con(child_script, child);
    }
  }
}

std::string TemplateResolver::resolve_mesh(const std::string& template_name) const {
  const auto it = template_to_mesh_.find(lower(template_name));
  if (it != template_to_mesh_.end()) return it->second;
  return find_mesh_global(lower(template_name));
}

}  // namespace bf2
