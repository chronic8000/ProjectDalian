#include <iostream>

#include "engine/formats/mesh/bf2_mesh.hpp"

int main() {
  const auto mesh = bf2::MeshLoader::load_from_file("tests/fixtures/cube.staticmesh");
  const auto extracted = bf2::MeshLoader::extract_geometry(mesh);
  std::cout << extracted.vertices.size() << '\n';
  return extracted.vertices.empty() ? 1 : 0;
}
