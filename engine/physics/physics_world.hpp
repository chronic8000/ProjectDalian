#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/formats/collision/bf2_collision.hpp"
#include "engine/formats/mesh/bf2_mesh.hpp"
#include "engine/formats/terrain/heightmap_cluster.hpp"
#include "engine/formats/terrain/terrain_loader.hpp"

namespace bf2 {

struct PhysicsBody {
  Float3 position{};
  Float3 velocity{};
  float radius = 0.5f;
  bool on_ground = false;
};

// A ground-clamped walker: horizontal desired velocity is applied and the body
// follows the terrain surface (with gravity when airborne).
struct CharacterController {
  Float3 position{};
  Float3 desired_velocity{};  // horizontal (x,z) drive; y ignored
  float eye_height = 1.8f;
  float vertical_velocity = 0.f;
  bool on_ground = false;
};

class PhysicsWorld {
public:
  // cell_size = world spacing between heightmap samples; centered maps the grid
  // onto world coordinates centred at the origin (BF2 convention).
  void set_terrain(const Terrain& terrain, float cell_size = 1.f, bool centered = false);
  // When set, terrain_height() samples the full 3x3 BF2 heightmap cluster (secondary
  // patches included) instead of the single merged/centered heightfield grid.
  void set_heightmap_cluster(const HeightmapCluster* cluster) { heightmap_cluster_ = cluster; }
  void add_static_collision(const CollisionMesh& mesh);
  void add_body(const PhysicsBody& body);
  void step(float delta_seconds);
  const std::vector<PhysicsBody>& bodies() const { return bodies_; }

  // World-space bilinear terrain height at (x, z).
  float terrain_height(float x, float z) const;

  // Add a world-space collision triangle (used for building floors/roofs/stairs).
  void add_collision_triangle(const Float3& a, const Float3& b, const Float3& c);
  // Build the spatial grid after all triangles have been added.
  void finalize_colliders();
  std::size_t collision_triangle_count() const { return tris_.size(); }

  // Highest walkable surface at (x,z) at or below feet+step_up, from terrain and
  // collision triangles. Returns the surface (foot) height.
  float support_height(float x, float z, float feet, float step_up) const;

  struct RayHit {
    bool hit = false;
    float distance = 0.f;
    Float3 point{};
    Float3 normal{};
  };
  // Cast a ray against collision triangles and the terrain; nearest hit wins.
  RayHit raycast(const Float3& origin, const Float3& dir, float max_dist) const;

  // Advance a character controller one step, clamping it to the terrain.
  void step_character(CharacterController& character, float delta_seconds) const;

private:
  struct Tri {
    Float3 a{};
    Float3 b{};
    Float3 c{};
  };

  static std::int64_t cell_key(int ix, int iz) {
    return (static_cast<std::int64_t>(ix) << 32) ^ (static_cast<std::uint32_t>(iz));
  }

  Terrain terrain_{};
  const HeightmapCluster* heightmap_cluster_ = nullptr;
  float cell_size_ = 1.f;
  bool centered_ = false;
  std::vector<CollisionMesh> static_colliders_;
  std::vector<PhysicsBody> bodies_;

  std::vector<Tri> tris_;
  std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid_;
  // Triangles too large to register per-cell without exploding the grid (carrier
  // decks, bridges, cliffs). Checked with a linear scan so they stay solid.
  std::vector<std::uint32_t> oversized_;
  float grid_cell_ = 8.f;
};

}  // namespace bf2
