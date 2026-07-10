#pragma once

#include "engine/formats/effects/effect_bundle.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace dalian {

struct Smoke;
struct Explosion;

// Attach retail EffectBundle library (call once after mounting Objects_client.zip).
void init_bf2_fx(bf2::EffectBundleLibrary* library);

// Spawn BF2-style particle bursts from retail tweaks when loaded, else tuned fallbacks.
void spawn_rocket_launch_fx(std::vector<Smoke>& smoke, const glm::vec3& origin,
                            const glm::vec3& dir);
// Continuous BF2 missile exhaust trail (e_missile_trail) — call each puff tick.
// density < 1 thins the trail (car-SAM) so the rocket body stays readable.
void spawn_missile_trail_fx(std::vector<Smoke>& smoke, const glm::vec3& pos, const glm::vec3& dir,
                            float density = 1.f);
void spawn_missile_detonation_fx(std::vector<Smoke>& smoke, std::vector<Explosion>& explosions,
                                 const glm::vec3& center, float scale = 1.f,
                                 const char* surface = "grass");
// Prefer retail Igla vehicle-explosion bundle when loaded.
void spawn_igla_detonation_fx(std::vector<Smoke>& smoke, std::vector<Explosion>& explosions,
                              const glm::vec3& center, float scale = 1.f);
void spawn_grenade_detonation_fx(std::vector<Smoke>& smoke, std::vector<Explosion>& explosions,
                                 const glm::vec3& center, const char* surface = "grass");
void spawn_jet_exhaust_fx(std::vector<Smoke>& smoke, const glm::vec3& pos, const glm::vec3& dir,
                          bool afterburner, int count = 1);
void spawn_muzzle_smoke_fx(std::vector<Smoke>& smoke, const glm::vec3& pos, const glm::vec3& dir);
void spawn_bullet_impact_fx(std::vector<Smoke>& smoke, const glm::vec3& pos,
                            const glm::vec3& normal, const char* surface = "grass");
void spawn_ambient_fx(std::vector<Smoke>& smoke, const char* bundle, const glm::vec3& pos,
                      const glm::vec3& dir, std::uint8_t kind_fallback = 0);

}  // namespace dalian
