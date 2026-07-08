#include "template_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Normalize a script `run` path to an archive vpath: forward slashes, strip a
// leading slash and an optional leading "objects/" segment (levels reference
// object cons as "/objects/..." but the Objects archive stores them without it).
std::string normalize_run_path(std::string path) {
  for (auto& c : path) {
    if (c == '\\') c = '/';
  }
  if (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  const std::string prefix = "objects/";
  if (lower(path).compare(0, prefix.size(), prefix) == 0) {
    path.erase(0, prefix.size());
  }
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

}  // namespace

TemplateResolver::TemplateResolver(ResourceManager& resources) : resources_(resources) {}

void TemplateResolver::build_from_static_objects(const std::string& static_objects_script) {
  std::istringstream in(static_objects_script);
  std::string line;
  while (std::getline(in, line)) {
    const auto tokens = tokenize(line);
    if (tokens.size() < 2) continue;
    if (lower(tokens[0]) != "run") continue;
    std::string con_vpath = normalize_run_path(tokens[1]);
    if (lower(con_vpath).rfind(".con") == std::string::npos) continue;
    if (std::find(visited_.begin(), visited_.end(), lower(con_vpath)) != visited_.end()) continue;
    visited_.push_back(lower(con_vpath));
    if (const auto bytes = resources_.read_bytes(con_vpath)) {
      const std::string script(reinterpret_cast<const char*>(bytes->data()), bytes->size());
      parse_object_con(script, con_vpath);
    }
  }
}

void TemplateResolver::parse_object_con(const std::string& script, const std::string& con_vpath) {
  const std::string folder = dir_of(con_vpath);
  std::istringstream in(script);
  std::string line;

  std::string current_template;
  std::unordered_map<std::string, std::string> template_geometry;  // lower(name) -> geom

  while (std::getline(in, line)) {
    const auto tokens = tokenize(line);
    if (tokens.size() < 2) continue;
    const std::string cmd = lower(tokens[0]);

    if (cmd == "objecttemplate.create" && tokens.size() >= 3) {
      current_template = lower(tokens[2]);
    } else if (cmd == "objecttemplate.geometry" && !current_template.empty()) {
      template_geometry[current_template] = tokens[1];
    }
  }

  for (const auto& [name, geom] : template_geometry) {
    // Preferred layout: <folder>/meshes/<geom>.staticmesh
    const std::string candidate = folder + "/meshes/" + geom + ".staticmesh";
    if (resources_.archives().exists(candidate)) {
      template_to_mesh_[name] = candidate;
      continue;
    }
    // Fallback: <folder>/<geom>.staticmesh
    const std::string flat = folder + "/" + geom + ".staticmesh";
    if (resources_.archives().exists(flat)) {
      template_to_mesh_[name] = flat;
    }
  }
}

std::string TemplateResolver::resolve_mesh(const std::string& template_name) const {
  const auto it = template_to_mesh_.find(lower(template_name));
  return it == template_to_mesh_.end() ? std::string() : it->second;
}

}  // namespace bf2
