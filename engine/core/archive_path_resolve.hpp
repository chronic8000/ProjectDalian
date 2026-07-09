#pragma once

#include "engine/formats/archive/archive.hpp"

#include <string>
#include <vector>

namespace bf2 {

// All archive vpaths to try for a mesh or other level asset reference.
// BF2 CompiledRoads.con uses Levels/<Map>/Roads/foo_compiled.mesh while
// client.zip stores Roads/foo_compiled.mesh at the archive root.
std::vector<std::string> archive_candidate_paths(const std::string& virtual_path);

// Folder hint for texture resolution beside a mesh (levels/foo/roads -> roads).
std::string mesh_texture_folder_hint(const std::string& mesh_vpath);

// First candidate path that exists in mounted archives (falls back to normalized input).
std::string resolve_mesh_vpath(const ArchiveMount& archives, const std::string& virtual_path);

bool archive_path_resolve_self_test();

}  // namespace bf2
