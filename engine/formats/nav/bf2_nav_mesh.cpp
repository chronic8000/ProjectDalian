#include "bf2_nav_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace bf2 {
namespace {

bool read_u32(const std::vector<std::uint8_t>& d, std::size_t off, std::uint32_t& out) {
  if (off + 4 > d.size()) return false;
  std::memcpy(&out, d.data() + off, 4);
  return true;
}

}  // namespace

bool NavMesh::load_from_bytes(const std::vector<std::uint8_t>& data) {
  vertices_.clear();
  triangles_.clear();
  adjacency_.clear();
  if (data.size() < 12) return false;

  std::uint32_t version = 0, vert_count = 0, tri_count = 0;
  if (!read_u32(data, 0, version) || !read_u32(data, 4, vert_count) || !read_u32(data, 8, tri_count))
    return false;
  if (version != 2 || vert_count == 0 || tri_count == 0) return false;

  const std::size_t vert_bytes = static_cast<std::size_t>(vert_count) * 12;
  const std::size_t tri_bytes = static_cast<std::size_t>(tri_count) * 12;
  if (12 + vert_bytes + tri_bytes > data.size()) return false;

  vertices_.resize(vert_count);
  std::memcpy(vertices_.data(), data.data() + 12, vert_bytes);

  triangles_.resize(tri_count);
  const std::uint8_t* tri_ptr = data.data() + 12 + vert_bytes;
  for (std::uint32_t i = 0; i < tri_count; ++i) {
    std::uint32_t a = 0, b = 0, c = 0;
    std::memcpy(&a, tri_ptr + i * 12 + 0, 4);
    std::memcpy(&b, tri_ptr + i * 12 + 4, 4);
    std::memcpy(&c, tri_ptr + i * 12 + 8, 4);
    if (a >= vert_count || b >= vert_count || c >= vert_count) continue;
    triangles_[i] = {a, b, c};
  }
  build_adjacency();
  return valid();
}

void NavMesh::build_adjacency() {
  adjacency_.assign(vertices_.size(), {});
  auto add_edge = [&](int a, int b) {
    if (a < 0 || b < 0 || a >= static_cast<int>(adjacency_.size()) ||
        b >= static_cast<int>(adjacency_.size()) || a == b)
      return;
    auto& la = adjacency_[static_cast<std::size_t>(a)];
    if (std::find(la.begin(), la.end(), b) == la.end()) la.push_back(b);
  };
  for (const auto& tri : triangles_) {
    add_edge(static_cast<int>(tri.x), static_cast<int>(tri.y));
    add_edge(static_cast<int>(tri.y), static_cast<int>(tri.z));
    add_edge(static_cast<int>(tri.z), static_cast<int>(tri.x));
  }
}

int NavMesh::nearest_vertex(const glm::vec3& p) const {
  if (vertices_.empty()) return -1;
  int best = 0;
  float best_d = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < vertices_.size(); ++i) {
    const float d = glm::dot(vertices_[i] - p, vertices_[i] - p);
    if (d < best_d) {
      best_d = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

std::vector<int> NavMesh::find_path(int from, int to) const {
  if (from < 0 || to < 0 || from >= static_cast<int>(vertices_.size()) ||
      to >= static_cast<int>(vertices_.size()) || from == to)
    return {from};

  struct Node {
    int v;
    float g;
    float f;
  };
  auto cmp = [](const Node& a, const Node& b) { return a.f > b.f; };
  std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
  std::unordered_map<int, int> came_from;
  std::unordered_map<int, float> g_score;
  g_score[from] = 0.f;
  open.push({from, 0.f, glm::length(vertices_[from] - vertices_[to])});

  while (!open.empty()) {
    const int cur = open.top().v;
    open.pop();
    if (cur == to) {
      std::vector<int> path;
      int c = to;
      while (true) {
        path.push_back(c);
        auto it = came_from.find(c);
        if (it == came_from.end()) break;
        c = it->second;
      }
      std::reverse(path.begin(), path.end());
      return path;
    }
    const float cur_g = g_score[cur];
    for (int nb : adjacency_[static_cast<std::size_t>(cur)]) {
      const float tentative = cur_g + glm::length(vertices_[cur] - vertices_[nb]);
      auto it = g_score.find(nb);
      if (it != g_score.end() && tentative >= it->second) continue;
      came_from[nb] = cur;
      g_score[nb] = tentative;
      open.push({nb, tentative, tentative + glm::length(vertices_[nb] - vertices_[to])});
    }
  }
  return {from};
}

glm::vec3 NavMesh::waypoint_along_path(const glm::vec3& from, const glm::vec3& to,
                                       float step) const {
  if (!valid()) return to;
  const int a = nearest_vertex(from);
  const int b = nearest_vertex(to);
  const auto path = find_path(a, b);
  if (path.size() < 2) return to;
  const glm::vec3 wp = vertices_[static_cast<std::size_t>(path[1])];
  const glm::vec3 delta = wp - from;
  const float len = glm::length(glm::vec2(delta.x, delta.z));
  if (len < 0.05f) return wp;
  if (len <= step) return wp;
  return from + glm::normalize(glm::vec3(delta.x, 0.f, delta.z)) * step;
}

}  // namespace bf2
