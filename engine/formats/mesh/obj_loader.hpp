#pragma once

#include "engine/formats/mesh/bf2_mesh.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

struct ObjMaterial {
  std::string name;
  float kd[3] = {0.75f, 0.75f, 0.75f};
};

// Wavefront OBJ → TexturedMeshData. Splits `usemtl` into submeshes; `base_map`
// is the material name (caller can bind solid-colour textures from MTL Kd).
TexturedMeshData load_obj_file(const std::string& path, const std::string& base_map = {});

// Parse sibling .mtl (or explicit path) for diffuse colours keyed by material name.
std::unordered_map<std::string, ObjMaterial> load_obj_materials(const std::string& mtl_path);

}  // namespace bf2
