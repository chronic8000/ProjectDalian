#include "effect_bundle.hpp"

#include "engine/core/resource_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string prop(const ObjectTemplate& t, const std::string& key) {
  auto it = t.properties.find(key);
  return it != t.properties.end() ? it->second : std::string();
}

bool has_sprite_props(const ObjectTemplate& t) {
  return t.properties.contains("ObjectTemplate.textureName") ||
         t.properties.contains("ObjectTemplate.emitSpeed");
}

}  // namespace

float Graph4::eval(float t) const {
  t = std::clamp(t, 0.f, 1.f);
  // Simple cubic Bezier approximation using control points as BF2 graphs do.
  const float u = 1.f - t;
  return u * u * u * y0 + 3.f * u * u * t * y0 + 3.f * u * t * t * y1 + t * t * t * y1;
}

glm::vec3 EffectBundleParser::parse_vec3(const std::string& s, const glm::vec3& def) {
  if (s.empty()) return def;
  float v[3] = {def.x, def.y, def.z};
  std::string t = s;
  for (char& c : t) {
    if (c == '/') c = ' ';
  }
  std::istringstream is(t);
  for (int i = 0; i < 3 && (is >> v[i]); ++i) {
  }
  return {v[0], v[1], v[2]};
}

float EffectBundleParser::parse_float(const std::string& s, float def) {
  if (s.empty()) return def;
  try {
    return std::stof(s);
  } catch (...) {
    return def;
  }
}

Graph4 EffectBundleParser::parse_graph(const std::string& s) {
  Graph4 g;
  if (s.empty()) return g;
  std::string t = s;
  for (char& c : t) {
    if (c == '/') c = ' ';
  }
  std::istringstream is(t);
  is >> g.x0 >> g.y0 >> g.x1 >> g.y1;
  return g;
}

std::string EffectBundleParser::normalize_tex_path(std::string p) {
  for (char& c : p) {
    if (c == '\\') c = '/';
  }
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  const std::string low = lower(p);
  if (low.size() > 4 && low.rfind(".dds") == low.size() - 4) {
    p.resize(p.size() - 4);
  }
  return p;
}

bool EffectBundleParser::parse_script(const std::string& script) {
  ConInterpreter interp;
  if (!interp.execute_script(script)) return false;
  ingest_templates(interp);
  return true;
}

void EffectBundleParser::ingest_templates(const ConInterpreter& interp) {
  for (const auto& [name, tmpl] : interp.templates()) {
    if (has_sprite_props(tmpl)) {
      SpriteParticleDef sp;
      sp.name = name;
      sp.emit_direction = parse_vec3(prop(tmpl, "ObjectTemplate.emitDirection"), sp.emit_direction);
      sp.emit_speed = parse_float(prop(tmpl, "ObjectTemplate.emitSpeed"), sp.emit_speed);
      sp.emit_frequency = parse_float(prop(tmpl, "ObjectTemplate.emitFrequency"), sp.emit_frequency);
      sp.time_to_live = parse_float(prop(tmpl, "ObjectTemplate.timeToLive"), sp.time_to_live);
      sp.particle_max_size = parse_float(prop(tmpl, "ObjectTemplate.particleMaxSize"), sp.particle_max_size);
      sp.air_resistance = parse_float(prop(tmpl, "ObjectTemplate.airResistance"), sp.air_resistance);
      sp.color1 = parse_vec3(prop(tmpl, "ObjectTemplate.color1"), sp.color1);
      sp.color2 = parse_vec3(prop(tmpl, "ObjectTemplate.color2"), sp.color2);
      sp.texture_path = normalize_tex_path(prop(tmpl, "ObjectTemplate.textureName"));
      sp.particle_type = prop(tmpl, "ObjectTemplate.particleType");
      sp.transparency_graph = parse_graph(prop(tmpl, "ObjectTemplate.transparencyGraph"));
      sp.size_graph = parse_graph(prop(tmpl, "ObjectTemplate.sizeGraph"));
      sp.gravity_graph = parse_graph(prop(tmpl, "ObjectTemplate.gravityGraph"));
      sp.animation_enable = parse_float(prop(tmpl, "ObjectTemplate.animationEnable")) > 0.5f;
      sp.animation_frame_count = static_cast<int>(parse_float(prop(tmpl, "ObjectTemplate.animationFrameCount")));
      sp.animation_frame_count_x =
          static_cast<int>(parse_float(prop(tmpl, "ObjectTemplate.animationFrameCountX"), 1.f));
      sprites_[name] = sp;
    }

    if (!tmpl.children.empty()) {
      EffectBundleDef bundle;
      bundle.name = name;
      bundle.child_templates = tmpl.children;
      for (const auto& child : tmpl.children) {
        if (auto it = sprites_.find(child); it != sprites_.end()) {
          bundle.sprite_systems.push_back(it->second);
        }
      }
      bundles_[name] = std::move(bundle);
    }
  }

  // Re-link sprite systems for bundles parsed after sprites in same file.
  for (auto& [name, bundle] : bundles_) {
    if (!bundle.sprite_systems.empty()) continue;
    bundle.sprite_systems.clear();
    for (const auto& child : bundle.child_templates) {
      if (auto it = sprites_.find(child); it != sprites_.end()) {
        bundle.sprite_systems.push_back(it->second);
      }
    }
  }
}

const EffectBundleDef* EffectBundleParser::find_bundle(const std::string& name) const {
  auto it = bundles_.find(name);
  return it != bundles_.end() ? &it->second : nullptr;
}

const SpriteParticleDef* EffectBundleParser::find_sprite(const std::string& name) const {
  auto it = sprites_.find(name);
  return it != sprites_.end() ? &it->second : nullptr;
}

void EffectBundleLibrary::clear() {
  bundles_.clear();
  sprites_.clear();
  loaded_paths_.clear();
}

bool EffectBundleLibrary::try_parse_path(ResourceManager& resources, const std::string& vpath,
                                         const std::string& bundle_name) {
  const auto bytes = resources.read_bytes(vpath);
  if (!bytes) return false;
  const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  EffectBundleParser parser;
  if (!parser.parse_script(text)) return false;
  const EffectBundleDef* bundle = parser.find_bundle(bundle_name);
  if (!bundle) return false;
  bundles_[bundle_name] = *bundle;
  for (const auto& sp : bundle->sprite_systems) {
    sprites_[sp.name] = sp;
  }
  for (const auto& [n, s] : parser.sprites()) {
    sprites_[n] = s;
  }
  loaded_paths_[bundle_name] = vpath;
  return true;
}

bool EffectBundleLibrary::load(ResourceManager& resources, const std::string& bundle_name) {
  if (bundles_.contains(bundle_name)) return true;

  const std::string low_name = lower(bundle_name);
  const std::vector<std::string> guesses = {
      "Effects/vehicles/MISC/e_jetexhaust/" + bundle_name + ".tweak",
      "Effects/weapons/muzzleflashes/emitters/" + bundle_name + "/" + bundle_name + ".tweak",
      "Effects/impacts/explosions/mexp/grenade/" + bundle_name + ".tweak",
  };
  for (const auto& g : guesses) {
    if (try_parse_path(resources, g, bundle_name)) return true;
  }

  // Brute search: list Effects/ tree (case-insensitive filename match).
  const auto entries = resources.archives().list("");
  for (const auto& e : entries) {
    const std::string el = lower(e);
    if (el.rfind("effects/") != 0 || el.rfind(".tweak") != el.size() - 6) continue;
    const auto slash = el.find_last_of('/');
    const std::string file = slash == std::string::npos ? el : el.substr(slash + 1);
    const std::string stem = file.substr(0, file.size() - 6);
    if (stem == low_name) {
      return try_parse_path(resources, e, bundle_name);
    }
  }
  return false;
}

const EffectBundleDef* EffectBundleLibrary::get(const std::string& bundle_name) const {
  auto it = bundles_.find(bundle_name);
  return it != bundles_.end() ? &it->second : nullptr;
}

const SpriteParticleDef* EffectBundleLibrary::sprite(const std::string& name) const {
  auto it = sprites_.find(name);
  return it != sprites_.end() ? &it->second : nullptr;
}

std::vector<EffectBundleLibrary::ParticleSpawn> EffectBundleLibrary::emit_burst(
    const std::string& bundle_name, const SpawnParams& p) const {
  std::vector<ParticleSpawn> out;
  const EffectBundleDef* bundle = get(bundle_name);
  if (!bundle || bundle->sprite_systems.empty()) return out;

  const glm::vec3 dir = glm::length(p.dir) > 1e-4f ? glm::normalize(p.dir) : glm::vec3(0.f, 0.f, -1.f);

  for (const SpriteParticleDef& sp : bundle->sprite_systems) {
    const int n = std::max(1, static_cast<int>(p.count * (sp.emit_frequency > 100.f ? 2 : 1)));
    for (int i = 0; i < n; ++i) {
      ParticleSpawn ps;
      ps.pos = p.pos;
      const float rand_a = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
      const float rand_b = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
      glm::vec3 emit_dir = sp.emit_direction;
      if (glm::length(emit_dir) < 1e-4f) emit_dir = glm::vec3(0.f, 0.f, -1.f);
      emit_dir = glm::normalize(emit_dir);
      // Transform emit direction to world using vehicle direction as -Z exhaust axis.
      const glm::vec3 world_emit = glm::normalize(dir * emit_dir.z + glm::vec3(emit_dir.x, emit_dir.y, 0.f));
      ps.vel = world_emit * (sp.emit_speed * (0.85f + rand_a * 0.3f)) * p.intensity;
      ps.life = sp.time_to_live * (0.9f + rand_b * 0.2f);
      ps.size = sp.particle_max_size * p.intensity;
      ps.color = glm::mix(sp.color1, sp.color2, rand_a);
      ps.texture_path = sp.texture_path;
      ps.additive = lower(sp.particle_type).find("additive") != std::string::npos;
      ps.transparency_graph = sp.transparency_graph;
      ps.size_graph = sp.size_graph;
      out.push_back(ps);
    }
  }
  return out;
}

}  // namespace bf2
