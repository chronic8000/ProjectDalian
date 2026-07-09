#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/resource_manager.hpp"

namespace bf2 {

// Resolves BF2 level placement template names to concrete mesh archive paths.
class TemplateResolver {
public:
  explicit TemplateResolver(ResourceManager& resources);

  // Follow `run` lines in StaticObjects.con (and nested object cons).
  void build_from_static_objects(const std::string& static_objects_script);

  // StaticObjects.con + DummyObjects.con when both are available.
  void build_from_level_scripts(const std::string& static_objects_script,
                                  const std::string& dummy_objects_script = {});

  std::string resolve_mesh(const std::string& template_name) const;
  const std::unordered_map<std::string, std::string>& map() const { return template_to_mesh_; }

private:
  struct TemplateDef {
    std::string geometry;
    std::string folder;
    std::vector<std::string> children;
  };

  void parse_object_con(const std::string& script, const std::string& con_vpath);
  void resolve_template_defs(const std::unordered_map<std::string, TemplateDef>& defs);
  std::string find_mesh_for_geometry(const std::string& folder, const std::string& geom) const;
  std::string find_mesh_global(const std::string& geom_lower) const;
  void ensure_mesh_index() const;
  void follow_run_script(const std::string& script);

  ResourceManager& resources_;
  std::unordered_map<std::string, std::string> template_to_mesh_;
  std::vector<std::string> visited_;
  mutable bool mesh_index_built_ = false;
  mutable std::unordered_map<std::string, std::string> mesh_by_stem_;
};

}  // namespace bf2
