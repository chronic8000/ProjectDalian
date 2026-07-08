#include <iostream>

#include "engine/formats/mesh/bf2_mesh.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: meshinfo <mesh.staticmesh>\n";
    return 1;
  }

  try {
    const auto mesh = bf2::MeshLoader::load_from_file(argv[1]);
    const auto info = bf2::MeshLoader::summarize(mesh);

    std::cout << "BF2 Respawn meshinfo\n";
    std::cout << "  version: " << info.version << '\n';
    std::cout << "  geometries: " << info.geometry_count << '\n';
    std::cout << "  lods: " << info.lod_count << '\n';
    std::cout << "  materials: " << info.material_count << '\n';
    std::cout << "  vertices: " << info.vertex_count << '\n';
    std::cout << "  indices: " << info.index_count << '\n';
    std::cout << "  vertex_stride: " << mesh.vertex_stride << '\n';
    std::cout << "  attributes: " << mesh.vertex_attributes.size() << '\n';
    for (std::size_t i = 0; i < mesh.vertex_attributes.size(); ++i) {
      const auto& attr = mesh.vertex_attributes[i];
      std::cout << "    attr[" << i << "] flag=" << attr.flag << " offset=" << attr.offset
                << " type=" << attr.vartype << " usage=" << attr.usage << '\n';
    }

    for (std::size_t g = 0; g < mesh.geometries.size(); ++g) {
      const auto& geometry = mesh.geometries[g];
      std::cout << "  geometry[" << g << "] lods=" << geometry.lods.size() << '\n';
      for (std::size_t l = 0; l < geometry.lods.size(); ++l) {
        const auto& lod = geometry.lods[l];
        std::cout << "    lod[" << l << "] materials=" << lod.materials.size() << '\n';
        for (std::size_t m = 0; m < lod.materials.size(); ++m) {
          const auto& material = lod.materials[m];
          std::cout << "      mat[" << m << "] fx=" << material.fx_file
                    << " technique=" << material.technique << " vstart=" << material.vertex_start
                    << " verts=" << material.vertex_count << " istart=" << material.index_start
                    << " indices=" << material.index_count << '\n';
          for (std::size_t k = 0; k < material.maps.size(); ++k) {
            std::cout << "        map[" << k << "] = " << material.maps[k] << '\n';
          }
        }
      }
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return 2;
  }
}
