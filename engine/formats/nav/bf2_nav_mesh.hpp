#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace bf2 {

// Battlefield 2 nav mesh from level GTSData/output/Infantry.vbf (version 2).
class NavMesh {
public:
  bool load_from_bytes(const std::vector<std::uint8_t>& data);

  bool valid() const { return !vertices_.empty() && !triangles_.empty(); }
  std::size_t vertex_count() const { return vertices_.size(); }
  std::size_t triangle_count() const { return triangles_.size(); }
  const std::vector<glm::vec3>& vertices() const { return vertices_; }

  int nearest_vertex(const glm::vec3& p) const;
  std::vector<int> find_path(int from, int to) const;
  glm::vec3 waypoint_along_path(const glm::vec3& from, const glm::vec3& to, float step) const;

private:
  std::vector<glm::vec3> vertices_;
  std::vector<glm::uvec3> triangles_;
  std::vector<std::vector<int>> adjacency_;

  void build_adjacency();
};

}  // namespace bf2
