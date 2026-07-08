#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/resource_manager.hpp"

namespace bf2 {

// Resolves BF2 level placement template names to concrete mesh archive paths.
//
// A level's StaticObjects.con `run`s a per-object `.con` for each template it
// uses. Each of those cons declares `ObjectTemplate.create <type> <name>` and
// `ObjectTemplate.geometry <geom>`; the geometry's mesh lives next to the con at
// `<con folder>/meshes/<geom>.staticmesh`. This class walks those cons (read
// from the mounted Objects archive) and builds a template -> mesh-path map.
class TemplateResolver {
public:
    explicit TemplateResolver(ResourceManager& resources);

    // Parse a StaticObjects.con script, following its `run` lines to build the
    // template -> mesh map. Safe to call once per level.
    void build_from_static_objects(const std::string& static_objects_script);

    // Returns the archive vpath of the template's static mesh, or "" if unknown.
    std::string resolve_mesh(const std::string& template_name) const;

    const std::unordered_map<std::string, std::string>& map() const { return template_to_mesh_; }

private:
    void parse_object_con(const std::string& script, const std::string& con_vpath);

    ResourceManager& resources_;
    std::unordered_map<std::string, std::string> template_to_mesh_;
    std::vector<std::string> visited_;
};

}  // namespace bf2
