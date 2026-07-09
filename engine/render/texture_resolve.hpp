#pragma once

#include "engine/core/resource_manager.hpp"

#include <optional>
#include <string>
#include <vector>

namespace bf2 {

// All archive vpaths to try when resolving a BF2 material texture path.
std::vector<std::string> texture_candidate_paths(const std::string& bf2_path,
                                                 const std::string& mesh_folder = {});

// First candidate that exists in mounted archives, or nullopt.
std::optional<std::string> resolve_texture_vpath(ResourceManager& resources,
                                                 const std::string& bf2_path,
                                                 const std::string& mesh_folder = {});

}  // namespace bf2
