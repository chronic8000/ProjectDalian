#include "weapon_profile.hpp"

#include <sstream>

namespace dalian {

WeaponProfile load_weapon_profile(bf2::ResourceManager& resources, const std::string& tweak_vpath) {
  WeaponProfile wp;
  wp.name = tweak_vpath;
  const auto bytes = resources.read_bytes(tweak_vpath);
  if (!bytes) return wp;
  std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  TweakData td;
  td.parse(text);
  auto apply = [&](TweakData& data) {
    if (data.has("ObjectTemplate.fire.rate"))
      wp.fire_rate = data.get_float("ObjectTemplate.fire.rate", wp.fire_rate);
    if (data.get_float("ObjectTemplate.fire.rate", 0.f) <= 0.f &&
        data.has("ObjectTemplate.fire.roundsPerMinute")) {
      const float rpm = data.get_float("ObjectTemplate.fire.roundsPerMinute", 0.f);
      if (rpm > 1.f) wp.fire_rate = 60.f / rpm;
    }
    if (data.has("ObjectTemplate.fire.burstRate"))
      wp.fire_rate = data.get_float("ObjectTemplate.fire.burstRate", wp.fire_rate);
    wp.damage = data.get_float("ObjectTemplate.damage.damage", wp.damage);
    wp.spread = data.get_float("ObjectTemplate.fire.deviation", wp.spread);
    wp.min_deviation = data.get_float("ObjectTemplate.fire.minDeviation", wp.min_deviation);
    wp.max_deviation = data.get_float("ObjectTemplate.fire.maxDeviation", wp.max_deviation);
    if (data.has("ObjectTemplate.deviation.minDev"))
      wp.min_deviation = data.get_float("ObjectTemplate.deviation.minDev", wp.min_deviation) * 0.001f;
    wp.magazine_size = data.get_int("ObjectTemplate.magazineSize", wp.magazine_size);
    wp.reserve_ammo = data.get_int("ObjectTemplate.ammo", wp.reserve_ammo);
    wp.reload_time = data.get_float("ObjectTemplate.reloadTime", wp.reload_time);
    if (wp.reload_time <= 0.f) wp.reload_time = data.get_float("ObjectTemplate.reloadtime", wp.reload_time);
    wp.tracer_speed = data.get_float("ObjectTemplate.tracer.speed", wp.tracer_speed);
    wp.tracer_count = data.get_int("ObjectTemplate.tracer.count", wp.tracer_count);
    wp.burst_size = std::max(1, data.get_int("ObjectTemplate.fire.burstSize", wp.burst_size));
    wp.burst_shot_rate = data.get_float("ObjectTemplate.fire.burstRate", wp.burst_shot_rate);
    wp.burst_pause = data.get_float("ObjectTemplate.fire.burstPause", wp.burst_pause);
    const std::string tracer_tpl = data.get_string("ObjectTemplate.tracer.template");
    if (!tracer_tpl.empty()) {
      wp.tracer_mesh = "effects/weapons/tracers/geometry/" + tracer_tpl + "/meshes/" + tracer_tpl +
                       ".bundledmesh";
    }
    const std::string muzz = data.get_string("ObjectTemplate.fire.effect");
    if (!muzz.empty()) wp.muzzle_effect = muzz;
  };
  apply(td);
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd, a;
    ls >> cmd >> a;
    if (cmd == "ObjectTemplate.addTemplate" && !a.empty()) {
      const auto slash = tweak_vpath.find_last_of('/');
      if (slash == std::string::npos) continue;
      const std::string folder = tweak_vpath.substr(0, slash);
      for (const char* ext : {".tweak", ".con"}) {
        if (const auto child = resources.read_bytes(folder + "/" + a + ext)) {
          TweakData child_td;
          child_td.parse(std::string(reinterpret_cast<const char*>(child->data()), child->size()));
          apply(child_td);
          break;
        }
      }
    }
  }
  wp.valid = td.has("ObjectTemplate.fire.rate") || td.has("ObjectTemplate.damage.damage") ||
             wp.fire_rate != 0.1f;
  return wp;
}

}  // namespace dalian
