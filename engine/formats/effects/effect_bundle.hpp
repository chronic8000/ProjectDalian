#pragma once

#include "engine/script/con_interpreter.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

class ResourceManager;

// BF2 cubic graph "x0/y0/x1/y1" — evaluated at t in [0,1].
struct Graph4 {
  float x0 = 0.f, y0 = 0.f, x1 = 1.f, y1 = 1.f;
  float eval(float t) const;
};

struct SpriteParticleDef {
  std::string name;
  glm::vec3 emit_direction{0.f, 0.f, -1.f};
  float emit_speed = 10.f;
  float emit_frequency = 60.f;
  float time_to_live = 1.f;
  float particle_max_size = 1.f;
  float air_resistance = 0.f;
  glm::vec3 color1{1.f};
  glm::vec3 color2{1.f};
  std::string texture_path;  // normalized forward slashes, no extension
  std::string particle_type; // Additive, Sorted, ...
  Graph4 transparency_graph;
  Graph4 size_graph;
  Graph4 gravity_graph;
  bool animation_enable = false;
  int animation_frame_count = 0;
  int animation_frame_count_x = 1;
};

struct EffectBundleDef {
  std::string name;
  std::vector<std::string> child_templates;
  std::vector<SpriteParticleDef> sprite_systems;
};

// Parse EffectBundle / SpriteParticleSystem blocks from a .tweak script.
class EffectBundleParser {
public:
  bool parse_script(const std::string& script);
  const EffectBundleDef* find_bundle(const std::string& name) const;
  const SpriteParticleDef* find_sprite(const std::string& name) const;
  const std::unordered_map<std::string, EffectBundleDef>& bundles() const { return bundles_; }
  const std::unordered_map<std::string, SpriteParticleDef>& sprites() const { return sprites_; }

private:
  std::unordered_map<std::string, EffectBundleDef> bundles_;
  std::unordered_map<std::string, SpriteParticleDef> sprites_;
  void ingest_templates(const ConInterpreter& interp);
  static glm::vec3 parse_vec3(const std::string& s, const glm::vec3& def = {});
  static float parse_float(const std::string& s, float def = 0.f);
  static Graph4 parse_graph(const std::string& s);
  static std::string normalize_tex_path(std::string p);
};

// Cached loader: reads tweak files from ResourceManager by effect name.
class EffectBundleLibrary {
public:
  void clear();
  // Load e.g. "e_jetexhaust_AB" — searches Effects/**/*.tweak in mounted archives.
  bool load(ResourceManager& resources, const std::string& bundle_name);
  const EffectBundleDef* get(const std::string& bundle_name) const;
  const SpriteParticleDef* sprite(const std::string& name) const;

  // Spawn particles from a bundle at world position/direction.
  struct SpawnParams {
    glm::vec3 pos{};
    glm::vec3 dir{0.f, 0.f, -1.f};
    int count = 1;
    float intensity = 1.f;
  };
  struct ParticleSpawn {
    glm::vec3 pos{};
    glm::vec3 vel{};
    float life = 1.f;
    float size = 1.f;
    glm::vec3 color{1.f};
    std::string texture_path;
    bool additive = true;
    Graph4 transparency_graph;
    Graph4 size_graph;
  };
  std::vector<ParticleSpawn> emit_burst(const std::string& bundle_name, const SpawnParams& p) const;

private:
  std::unordered_map<std::string, EffectBundleDef> bundles_;
  std::unordered_map<std::string, SpriteParticleDef> sprites_;
  std::unordered_map<std::string, std::string> loaded_paths_;
  bool try_parse_path(ResourceManager& resources, const std::string& vpath,
                      const std::string& bundle_name);
};

}  // namespace bf2
