#include "ambient_emitter.hpp"

#include "bf2_effects.hpp"

#include <algorithm>
#include <cctype>

namespace dalian {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool is_ambient_emitter_template(const std::string& template_name) {
  const std::string n = lower(template_name);
  if (n.rfind("amdstat", 0) == 0) return true;
  if (n.rfind("ambstat", 0) == 0) return true;
  if (n.rfind("baselight", 0) == 0) return true;
  if (n.find("waterspray") != std::string::npos) return true;
  if (n.find("factoryneon") != std::string::npos) return true;
  if (n.rfind("glow", 0) == 0) return true;
  return false;
}

std::string ambient_emitter_bundle(const std::string& template_name) {
  const std::string n = lower(template_name);
  if (n.find("fountain") != std::string::npos || n.find("waterspray") != std::string::npos) {
    return "e_sAmb_Fountain_waterSpray";
  }
  if (n.find("fire") != std::string::npos || n.find("oiltower") != std::string::npos) {
    return "e_sAmb_fire";
  }
  if (n.find("smoke") != std::string::npos) return "e_sAmb_OnlySmoke";
  if (n.rfind("baselight", 0) == 0 || n.rfind("glow", 0) == 0 ||
      n.find("factoryneon") != std::string::npos) {
    return "";
  }
  return "e_sAmb_OnlySmoke";
}

std::vector<AmbientEmitter> collect_ambient_emitters(
    const std::vector<bf2::ObjectInstance>& placements) {
  std::vector<AmbientEmitter> out;
  out.reserve(64);
  for (const auto& ent : placements) {
    if (!is_ambient_emitter_template(ent.template_name)) continue;
    AmbientEmitter e;
    e.pos = glm::vec3(ent.position[0], ent.position[1], ent.position[2]);
    const std::string n = lower(ent.template_name);
    const std::string bundle = ambient_emitter_bundle(ent.template_name);
    if (bundle.empty()) {
      e.kind = 2;
      e.emit_period = 1.f / 8.f;
      if (n.find("red") != std::string::npos) e.light_color = {1.f, 0.25f, 0.15f};
      else if (n.find("blue") != std::string::npos) e.light_color = {0.35f, 0.55f, 1.f};
      else if (n.find("green") != std::string::npos) e.light_color = {0.4f, 1.f, 0.45f};
      if (n.find("runway") != std::string::npos) {
        e.light_color = {1.f, 0.95f, 0.75f};
        e.light_radius = 22.f;
      }
    } else if (bundle.find("Fountain") != std::string::npos) {
      e.bundle = bundle;
      e.kind = 1;
      e.emit_period = 1.f / 45.f;
    } else {
      e.bundle = bundle;
      e.kind = 0;
      e.emit_period = 1.f / 24.f;
    }
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<SceneLight> collect_scene_lights(const std::vector<AmbientEmitter>& emitters) {
  std::vector<SceneLight> lights;
  lights.reserve(emitters.size());
  for (const auto& e : emitters) {
    if (e.kind != 2) continue;
    SceneLight sl;
    sl.pos = e.pos;
    sl.color = e.light_color;
    sl.radius = e.light_radius;
    lights.push_back(sl);
  }
  return lights;
}

void step_ambient_emitters(std::vector<AmbientEmitter>& emitters, std::vector<Smoke>& smoke,
                           float dt) {
  for (auto& e : emitters) {
    e.accum += dt;
    while (e.accum >= e.emit_period) {
      e.accum -= e.emit_period;
      if (e.kind == 2) {
        // Baselights are drawn as soft camera-facing glow sprites — do not also
        // emit kind-2 smoke puffs (those became the white vertical streaks).
        continue;
      }
      if (!e.bundle.empty()) {
        spawn_ambient_fx(smoke, e.bundle.c_str(), e.pos, e.dir, e.kind);
      }
    }
  }
}

bool ambient_emitter_self_test() {
  if (!is_ambient_emitter_template("amdStat_Fountain_waterSpray")) return false;
  if (is_ambient_emitter_template("ch_house_01")) return false;
  const std::string b = ambient_emitter_bundle("amdStat_Fountain_waterSpray");
  if (b.find("Fountain") == std::string::npos) return false;
  bf2::ObjectInstance ent;
  ent.template_name = "ambstat_OnlySmoke";
  ent.position[0] = 1.f;
  ent.position[1] = 2.f;
  ent.position[2] = 3.f;
  const auto list = collect_ambient_emitters({ent});
  return list.size() == 1 && list[0].pos.y == 2.f;
}

}  // namespace dalian
