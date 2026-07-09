#include "bf2_effects.hpp"

#include "game_sim_types.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dalian {
namespace {

bf2::EffectBundleLibrary* g_fx = nullptr;

float frand() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); }
glm::vec3 rand_dir() {
  const float a = frand() * 6.2831853f;
  const float b = (frand() - 0.5f) * 1.2f;
  return glm::normalize(glm::vec3(std::cos(a) * std::cos(b), std::sin(b) * 0.35f + 0.15f,
                                  std::sin(a) * std::cos(b)));
}

void push_smoke(std::vector<Smoke>& smoke, const glm::vec3& p, const glm::vec3& vel, float life,
                float size, std::uint8_t kind, const glm::vec3& tint = {},
                const bf2::Graph4* transp = nullptr, const bf2::Graph4* sz = nullptr) {
  if (smoke.size() >= 8000) return;
  Smoke s;
  s.p = p;
  s.vel = vel;
  s.age = 0.f;
  s.life = life;
  s.size = size;
  s.birth_size = size;
  s.kind = kind;
  s.tint = tint;
  if (transp && sz) {
    s.use_graphs = true;
    s.transp_graph = *transp;
    s.size_graph = *sz;
  }
  smoke.push_back(s);
}

void emit_bundle(std::vector<Smoke>& smoke, const char* bundle, const glm::vec3& pos,
                 const glm::vec3& dir, int count, float intensity, std::uint8_t kind_fallback) {
  if (!g_fx) return;
  bf2::EffectBundleLibrary::SpawnParams sp;
  sp.pos = pos;
  sp.dir = dir;
  sp.count = count;
  sp.intensity = intensity;
  for (const auto& ps : g_fx->emit_burst(bundle, sp)) {
    const std::uint8_t kind =
        ps.additive ? (intensity > 1.2f ? static_cast<std::uint8_t>(2) : kind_fallback) : 0;
    push_smoke(smoke, ps.pos, ps.vel, ps.life, ps.size, kind, ps.color, &ps.transparency_graph,
               &ps.size_graph);
  }
}

const char* surface_mexp(const char* surface) {
  if (!surface) return "e_mexp_grenade_grass";
  std::string s(surface);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s.find("water") != std::string::npos) return "e_mexp_grenade_water";
  if (s.find("sand") != std::string::npos) return "e_mexp_grenade_sand";
  if (s.find("rock") != std::string::npos) return "e_mexp_grenade_rock";
  if (s.find("mud") != std::string::npos) return "e_mexp_grenade_Mud";
  if (s.find("tarmac") != std::string::npos || s.find("road") != std::string::npos)
    return "e_mexp_grenade_tarmac";
  if (s.find("gravel") != std::string::npos) return "e_mexp_grenade_gravel";
  if (s.find("dirt") != std::string::npos) return "e_mexp_grenade_dirt";
  return "e_mexp_grenade_grass";
}

}  // namespace

void init_bf2_fx(bf2::EffectBundleLibrary* library) { g_fx = library; }

void spawn_rocket_launch_fx(std::vector<Smoke>& smoke, const glm::vec3& origin,
                            const glm::vec3& dir) {
  if (g_fx && g_fx->get("e_muzz_rocketpod")) {
    emit_bundle(smoke, "e_muzz_rocketpod", origin, dir, 8, 1.f, 1);
    return;
  }
  const glm::vec3 back = glm::normalize(dir) * -1.f;
  for (int i = 0; i < 14; ++i) {
    push_smoke(smoke, origin + back * (0.1f + frand() * 0.4f),
               back * (8.f + frand() * 10.f) + rand_dir() * 2.f, 0.5f + frand() * 0.35f,
               0.35f + frand() * 0.25f, 1);
  }
}

void spawn_missile_detonation_fx(std::vector<Smoke>& smoke, std::vector<Explosion>& explosions,
                                 const glm::vec3& center, float scale, const char* surface) {
  Explosion ex;
  ex.p = center;
  ex.age = 0.f;
  ex.life = 0.9f * scale;
  ex.scale = scale;
  explosions.push_back(ex);

  const char* bundle = surface_mexp(surface);
  if (g_fx && g_fx->get(bundle)) {
    emit_bundle(smoke, bundle, center, glm::vec3(0.f, 1.f, 0.f),
                static_cast<int>(28 * scale), scale, 3);
    return;
  }

  for (int i = 0; i < 28; ++i) {
    const glm::vec3 v = rand_dir() * (6.f + frand() * 18.f);
    push_smoke(smoke, center + v * 0.04f, v, 0.8f + frand() * 1.4f, 0.5f + frand() * 1.2f, 3);
  }
  for (int i = 0; i < 18; ++i) {
    push_smoke(smoke, center, rand_dir() * (2.f + frand() * 5.f), 2.f + frand() * 2.5f,
               0.9f + frand() * 1.6f, 0);
  }
}

void spawn_grenade_detonation_fx(std::vector<Smoke>& smoke, std::vector<Explosion>& explosions,
                                 const glm::vec3& center, const char* surface) {
  spawn_missile_detonation_fx(smoke, explosions, center, 0.75f, surface);
}

void spawn_jet_exhaust_fx(std::vector<Smoke>& smoke, const glm::vec3& pos, const glm::vec3& dir,
                          bool afterburner, int count) {
  if (afterburner && g_fx && g_fx->get("e_jetexhaust_AB")) {
    emit_bundle(smoke, "e_jetexhaust_AB", pos, dir, count * 2, 1.4f, 2);
    return;
  }
  const glm::vec3 back = glm::length(dir) > 1e-4f ? -glm::normalize(dir) : glm::vec3(0.f, 0.f, -1.f);
  const float spd = afterburner ? 55.f : 28.f;
  const float life = afterburner ? 0.12f : 0.22f;
  const float sz = afterburner ? 0.55f : 0.32f;
  const std::uint8_t kind = afterburner ? 2 : 1;
  for (int i = 0; i < count; ++i) {
    push_smoke(smoke, pos + rand_dir() * 0.08f, back * (spd + frand() * 12.f), life, sz, kind);
  }
}

void spawn_muzzle_smoke_fx(std::vector<Smoke>& smoke, const glm::vec3& pos, const glm::vec3& dir) {
  const glm::vec3 fwd = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(0.f, 0.f, 1.f);
  for (int i = 0; i < 3; ++i) {
    push_smoke(smoke, pos, fwd * (12.f + frand() * 8.f) + rand_dir() * 1.5f, 0.08f, 0.18f, 1);
  }
}

void spawn_bullet_impact_fx(std::vector<Smoke>& smoke, const glm::vec3& pos,
                            const glm::vec3& normal, const char* surface) {
  const char* bundle = "e_bhit_grass";
  if (surface) {
    if (std::strcmp(surface, "metal") == 0) bundle = "e_bhit_metal";
    else if (std::strcmp(surface, "concrete") == 0) bundle = "e_bhit_concrete";
    else if (std::strcmp(surface, "sand") == 0) bundle = "e_bhit_sand";
    else if (std::strcmp(surface, "water") == 0) bundle = "e_bhit_water";
  }
  if (g_fx && g_fx->get(bundle)) {
    emit_bundle(smoke, bundle, pos, normal, 4, 0.35f, 2);
    return;
  }
  for (int i = 0; i < 4; ++i) {
    push_smoke(smoke, pos, normal * (2.f + frand() * 3.f) + rand_dir() * 1.2f, 0.25f, 0.12f, 2);
  }
}

void spawn_ambient_fx(std::vector<Smoke>& smoke, const char* bundle, const glm::vec3& pos,
                      const glm::vec3& dir, std::uint8_t kind_fallback) {
  if (g_fx && bundle && g_fx->get(bundle)) {
    emit_bundle(smoke, bundle, pos, dir, 2, 0.85f, kind_fallback);
    return;
  }
  push_smoke(smoke, pos, dir * 0.5f + rand_dir() * 0.4f, 1.2f, 0.35f, kind_fallback);
}

}  // namespace dalian
