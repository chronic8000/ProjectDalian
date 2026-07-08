#include "physics_world.hpp"

#include <algorithm>
#include <cmath>

namespace bf2 {
namespace {

inline Float3 sub(const Float3& a, const Float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Float3 add3(const Float3& a, const Float3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Float3 scale3(const Float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot3(const Float3& a, const Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Float3 cross3(const Float3& a, const Float3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool finite3(const Float3& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// Closest point on triangle abc to point p (Ericson, Real-Time Collision Detection).
Float3 closest_on_triangle(const Float3& p, const Float3& a, const Float3& b, const Float3& c) {
  const Float3 ab = sub(b, a);
  const Float3 ac = sub(c, a);
  const Float3 ap = sub(p, a);
  const float d1 = dot3(ab, ap);
  const float d2 = dot3(ac, ap);
  if (d1 <= 0.f && d2 <= 0.f) return a;
  const Float3 bp = sub(p, b);
  const float d3 = dot3(ab, bp);
  const float d4 = dot3(ac, bp);
  if (d3 >= 0.f && d4 <= d3) return b;
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
    const float v = d1 / (d1 - d3);
    return add3(a, scale3(ab, v));
  }
  const Float3 cp = sub(p, c);
  const float d5 = dot3(ab, cp);
  const float d6 = dot3(ac, cp);
  if (d6 >= 0.f && d5 <= d6) return c;
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
    const float w = d2 / (d2 - d6);
    return add3(a, scale3(ac, w));
  }
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return add3(b, scale3(sub(c, b), w));
  }
  const float denom = 1.f / (va + vb + vc);
  const float v = vb * denom;
  const float w = vc * denom;
  return add3(a, add3(scale3(ab, v), scale3(ac, w)));
}

}  // namespace

void PhysicsWorld::set_terrain(const Terrain& terrain, float cell_size, bool centered) {
  terrain_ = terrain;
  cell_size_ = cell_size > 0.f ? cell_size : 1.f;
  centered_ = centered;
}

void PhysicsWorld::add_static_collision(const CollisionMesh& mesh) {
  static_colliders_.push_back(mesh);
}

void PhysicsWorld::add_collision_triangle(const Float3& a, const Float3& b, const Float3& c) {
  if (!finite3(a) || !finite3(b) || !finite3(c)) {
    return;  // reject NaN/inf so the spatial grid can't blow up
  }
  tris_.push_back({a, b, c});
}

void PhysicsWorld::finalize_colliders() {
  grid_.clear();
  // A single triangle spanning too many cells (e.g. cliff/harbor-scale geometry)
  // would explode the grid; skip those to bound memory. Buildings/props are small.
  constexpr int kMaxCellsPerTri = 256;
  for (std::uint32_t i = 0; i < tris_.size(); ++i) {
    const Tri& t = tris_[i];
    const float min_x = std::min({t.a.x, t.b.x, t.c.x});
    const float max_x = std::max({t.a.x, t.b.x, t.c.x});
    const float min_z = std::min({t.a.z, t.b.z, t.c.z});
    const float max_z = std::max({t.a.z, t.b.z, t.c.z});
    const int ix0 = static_cast<int>(std::floor(min_x / grid_cell_));
    const int ix1 = static_cast<int>(std::floor(max_x / grid_cell_));
    const int iz0 = static_cast<int>(std::floor(min_z / grid_cell_));
    const int iz1 = static_cast<int>(std::floor(max_z / grid_cell_));
    const long long span =
        static_cast<long long>(ix1 - ix0 + 1) * static_cast<long long>(iz1 - iz0 + 1);
    if (span <= 0 || span > kMaxCellsPerTri) {
      continue;
    }
    for (int ix = ix0; ix <= ix1; ++ix) {
      for (int iz = iz0; iz <= iz1; ++iz) {
        grid_[cell_key(ix, iz)].push_back(i);
      }
    }
  }
}

float PhysicsWorld::support_height(float x, float z, float feet, float step_up) const {
  float best = terrain_height(x, z);
  if (grid_.empty()) {
    return best;
  }
  const int ix = static_cast<int>(std::floor(x / grid_cell_));
  const int iz = static_cast<int>(std::floor(z / grid_cell_));
  const auto it = grid_.find(cell_key(ix, iz));
  if (it == grid_.end()) {
    return best;
  }
  const float ceiling = feet + step_up;
  for (const std::uint32_t idx : it->second) {
    const Tri& t = tris_[idx];
    // Barycentric point-in-triangle in XZ.
    const float d = (t.b.z - t.c.z) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.z - t.c.z);
    if (std::fabs(d) < 1e-6f) {
      continue;
    }
    const float u = ((t.b.z - t.c.z) * (x - t.c.x) + (t.c.x - t.b.x) * (z - t.c.z)) / d;
    const float v = ((t.c.z - t.a.z) * (x - t.c.x) + (t.a.x - t.c.x) * (z - t.c.z)) / d;
    const float w = 1.0f - u - v;
    if (u < -0.01f || v < -0.01f || w < -0.01f) {
      continue;
    }
    const float h = u * t.a.y + v * t.b.y + w * t.c.y;
    if (h <= ceiling && h > best) {
      best = h;
    }
  }
  return best;
}

PhysicsWorld::RayHit PhysicsWorld::raycast(const Float3& origin, const Float3& dir,
                                           float max_dist) const {
  RayHit best;
  const float dlen = std::sqrt(dot3(dir, dir));
  if (dlen < 1e-6f) {
    return best;
  }
  const Float3 d{dir.x / dlen, dir.y / dlen, dir.z / dlen};
  best.distance = max_dist;

  // Triangles: gather unique candidates from grid cells sampled along the ray,
  // then do exact Moller-Trumbore intersection.
  if (!grid_.empty()) {
    const float step = grid_cell_ * 0.5f;
    for (float s = 0.f; s <= max_dist; s += step) {
      const float px = origin.x + d.x * s;
      const float pz = origin.z + d.z * s;
      const int ix = static_cast<int>(std::floor(px / grid_cell_));
      const int iz = static_cast<int>(std::floor(pz / grid_cell_));
      for (int dix = -1; dix <= 1; ++dix) {
        for (int diz = -1; diz <= 1; ++diz) {
          const std::int64_t key = cell_key(ix + dix, iz + diz);
          const auto it = grid_.find(key);
          if (it == grid_.end()) {
            continue;
          }
          for (const std::uint32_t idx : it->second) {
            const Tri& t = tris_[idx];
            const Float3 e1 = sub(t.b, t.a);
            const Float3 e2 = sub(t.c, t.a);
            const Float3 pv = cross3(d, e2);
            const float det = dot3(e1, pv);
            if (std::fabs(det) < 1e-8f) {
              continue;
            }
            const float inv = 1.f / det;
            const Float3 tv = sub(origin, t.a);
            const float u = dot3(tv, pv) * inv;
            if (u < 0.f || u > 1.f) {
              continue;
            }
            const Float3 qv = cross3(tv, e1);
            const float v = dot3(d, qv) * inv;
            if (v < 0.f || u + v > 1.f) {
              continue;
            }
            const float tt = dot3(e2, qv) * inv;
            if (tt > 0.01f && tt < best.distance) {
              best.hit = true;
              best.distance = tt;
              best.point = {origin.x + d.x * tt, origin.y + d.y * tt, origin.z + d.z * tt};
              Float3 n = cross3(e1, e2);
              const float nl = std::sqrt(dot3(n, n));
              best.normal = nl > 1e-6f ? Float3{n.x / nl, n.y / nl, n.z / nl} : Float3{0, 1, 0};
            }
          }
        }
      }
    }
  }

  // Terrain: march and detect where the ray drops below the heightfield.
  {
    const float march = 0.5f;
    float prev_gap = origin.y - terrain_height(origin.x, origin.z);
    for (float s = march; s <= best.distance; s += march) {
      const float px = origin.x + d.x * s;
      const float py = origin.y + d.y * s;
      const float pz = origin.z + d.z * s;
      const float gap = py - terrain_height(px, pz);
      if (gap <= 0.f && prev_gap > 0.f) {
        const float frac = prev_gap / (prev_gap - gap);
        const float hitd = (s - march) + frac * march;
        if (hitd < best.distance) {
          best.hit = true;
          best.distance = hitd;
          best.point = {origin.x + d.x * hitd, origin.y + d.y * hitd, origin.z + d.z * hitd};
          best.normal = {0, 1, 0};
        }
        break;
      }
      prev_gap = gap;
    }
  }

  return best;
}

void PhysicsWorld::add_body(const PhysicsBody& body) { bodies_.push_back(body); }

float PhysicsWorld::terrain_height(float x, float z) const {
  if (terrain_.width == 0 || terrain_.height == 0) {
    return 0.f;
  }
  const float w = static_cast<float>(terrain_.width);
  const float h = static_cast<float>(terrain_.height);

  // World -> grid coordinates.
  float gx = x / cell_size_ + (centered_ ? w * 0.5f : 0.f);
  float gz = z / cell_size_ + (centered_ ? h * 0.5f : 0.f);
  gx = std::clamp(gx, 0.f, w - 1.001f);
  gz = std::clamp(gz, 0.f, h - 1.001f);

  const int x0 = static_cast<int>(gx);
  const int z0 = static_cast<int>(gz);
  const int x1 = std::min(x0 + 1, static_cast<int>(terrain_.width) - 1);
  const int z1 = std::min(z0 + 1, static_cast<int>(terrain_.height) - 1);
  const float fx = gx - x0;
  const float fz = gz - z0;

  auto at = [&](int cx, int cz) {
    return terrain_.samples[static_cast<std::size_t>(cz) * terrain_.width + cx].height;
  };
  const float h00 = at(x0, z0);
  const float h10 = at(x1, z0);
  const float h01 = at(x0, z1);
  const float h11 = at(x1, z1);
  const float top = h00 + (h10 - h00) * fx;
  const float bottom = h01 + (h11 - h01) * fx;
  return top + (bottom - top) * fz;
}

void PhysicsWorld::step(float delta_seconds) {
  constexpr float gravity = -9.81f;
  for (auto& body : bodies_) {
    body.velocity.y += gravity * delta_seconds;
    body.position.x += body.velocity.x * delta_seconds;
    body.position.y += body.velocity.y * delta_seconds;
    body.position.z += body.velocity.z * delta_seconds;

    const float ground = terrain_height(body.position.x, body.position.z) + body.radius;
    if (body.position.y < ground) {
      body.position.y = ground;
      body.velocity.y = 0.f;
      body.on_ground = true;
    } else {
      body.on_ground = false;
    }
  }
}

void PhysicsWorld::step_character(CharacterController& c, float delta_seconds) const {
  // Real earth gravity feels floaty for a ~1.8 m avatar moving this fast, so use
  // a game-tuned value that gives a snappy fall/jump arc.
  constexpr float gravity = -22.f;

  auto clamp_xz = [&]() {
    if (terrain_.width < 2 || terrain_.height < 2) {
      return;
    }
    const float margin = cell_size_ * 3.f;
    const float half_x = static_cast<float>(terrain_.width) * 0.5f * cell_size_;
    const float half_z = static_cast<float>(terrain_.height) * 0.5f * cell_size_;
    const float max_x =
        static_cast<float>(terrain_.width - 1) * cell_size_ - half_x - margin;
    const float max_z =
        static_cast<float>(terrain_.height - 1) * cell_size_ - half_z - margin;
    c.position.x = std::clamp(c.position.x, -half_x + margin, max_x);
    c.position.z = std::clamp(c.position.z, -half_z + margin, max_z);
  };

  constexpr float step_up = 0.7f;
  constexpr float radius = 0.4f;
  clamp_xz();
  c.position.x += c.desired_velocity.x * delta_seconds;
  c.position.z += c.desired_velocity.z * delta_seconds;
  clamp_xz();

  // Horizontal collision. Model the body as a vertical line, sampled at several
  // heights, and push out of any triangle whose contact point sits ABOVE the
  // step-up height (i.e. an obstacle you can't just step onto). Low contacts
  // (curbs, ramps, floors) are intentionally ignored here and resolved by the
  // vertical support pass, so slopes and bridges stay walkable.
  if (!grid_.empty()) {
    const float feet = c.position.y - c.eye_height;
    const float head = feet + c.eye_height;
    const float block_above = feet + step_up + 0.05f;
    // Sample knee/waist/chest so both low props and tall walls register.
    const float samples[3] = {feet + step_up + 0.2f, feet + 0.9f, feet + 1.5f};
    for (int iter = 0; iter < 6; ++iter) {
      bool pushed = false;
      const int ix = static_cast<int>(std::floor(c.position.x / grid_cell_));
      const int iz = static_cast<int>(std::floor(c.position.z / grid_cell_));
      for (int dix = -1; dix <= 1; ++dix) {
        for (int diz = -1; diz <= 1; ++diz) {
          const auto it = grid_.find(cell_key(ix + dix, iz + diz));
          if (it == grid_.end()) {
            continue;
          }
          for (const std::uint32_t idx : it->second) {
            const Tri& t = tris_[idx];
            const float tri_max_y = std::max({t.a.y, t.b.y, t.c.y});
            const float tri_min_y = std::min({t.a.y, t.b.y, t.c.y});
            if (tri_max_y < block_above || tri_min_y > head) {
              continue;  // fully steppable, or entirely overhead
            }
            for (const float sy : samples) {
              if (sy > tri_max_y + radius || sy < tri_min_y - radius) {
                continue;
              }
              const Float3 p{c.position.x, sy, c.position.z};
              const Float3 cp = closest_on_triangle(p, t.a, t.b, t.c);
              if (cp.y <= block_above) {
                continue;  // contact is low enough to step onto
              }
              const float ddx = p.x - cp.x;
              const float ddz = p.z - cp.z;
              const float d2 = ddx * ddx + ddz * ddz;
              if (d2 < radius * radius) {
                const float d = std::sqrt(d2);
                if (d > 1e-4f) {
                  const float push = radius - d;
                  c.position.x += (ddx / d) * push;
                  c.position.z += (ddz / d) * push;
                  pushed = true;
                }
              }
            }
          }
        }
      }
      if (!pushed) {
        break;
      }
    }
    clamp_xz();
  }

  const float feet_now = c.position.y - c.eye_height;
  const float ground = support_height(c.position.x, c.position.z, feet_now, step_up) + c.eye_height;

  c.vertical_velocity += gravity * delta_seconds;
  float new_y = c.position.y + c.vertical_velocity * delta_seconds;

  if (new_y <= ground) {
    // At or below the walking surface: snap up and land.
    new_y = ground;
    c.vertical_velocity = 0.f;
    c.on_ground = true;
  } else if (c.on_ground && c.vertical_velocity <= 0.f && (new_y - ground) < step_up + 0.3f) {
    // Small step/slope down while grounded: stick to the surface instead of
    // launching into the air.
    new_y = ground;
    c.vertical_velocity = 0.f;
    c.on_ground = true;
  } else {
    c.on_ground = false;
  }
  c.position.y = new_y;

  // Never let a degenerate contact leak NaN/inf into the camera matrices.
  if (!finite3(c.position)) {
    c.position = {0.f, terrain_height(0.f, 0.f) + c.eye_height + 2.f, 0.f};
    c.vertical_velocity = 0.f;
    c.on_ground = false;
  }
}

}  // namespace bf2
