#pragma once

#include "engine/formats/mesh/bf2_mesh.hpp"

#include <string>

namespace bf2 {

// Minimal Wavefront OBJ → TexturedMeshData for custom content (missiles, props).
// Supports v / vt / vn / f (tri + quad). Materials: optional base_map path override.
TexturedMeshData load_obj_file(const std::string& path, const std::string& base_map = {});

}  // namespace bf2
