#include "soldier_anim_library.hpp"

#include "soldier_anim.hpp"

#include "engine/formats/animation/bf2_animation.hpp"

#include <sstream>

namespace dalian {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string normalize_baf_path(std::string p) {
  for (char& c : p) {
    if (c == '\\') c = '/';
  }
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  const std::string prefix = "objects/";
  const std::string pl = lower(p);
  if (pl.rfind(prefix, 0) == 0) p = p.substr(prefix.size());
  return p;
}

std::string clip_key_from_path(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
  std::string stem = file;
  if (stem.size() > 4 && lower(stem).rfind(".baf") == stem.size() - 4) stem.resize(stem.size() - 4);
  return lower(stem);
}

}  // namespace

bool SoldierAnimLibrary::load_from_inc(bf2::ResourceManager& resources, const std::string& inc_vpath) {
  clips_.clear();
  const auto bytes = resources.read_bytes(inc_vpath);
  if (!bytes) return false;
  const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  std::istringstream in(text);
  std::string line;
  int loaded = 0;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd, path;
    if (!(ls >> cmd >> path)) continue;
    if (lower(cmd) != "animationsystem.createanimation") continue;
    path = normalize_baf_path(path);
    const auto baf = resources.read_bytes(path);
    if (!baf) continue;
    try {
      bf2::AnimationClip clip = bf2::AnimationLoader::load_from_memory(*baf);
      clips_[clip_key_from_path(path)] = std::move(clip);
      ++loaded;
    } catch (...) {
    }
  }
  return loaded > 0;
}

bool SoldierAnimLibrary::has(const std::string& clip_name) const {
  return clips_.contains(lower(clip_name));
}

const bf2::AnimationClip* SoldierAnimLibrary::get(const std::string& clip_name) const {
  const auto it = clips_.find(lower(clip_name));
  return it == clips_.end() ? nullptr : &it->second;
}

const bf2::AnimationClip* SoldierAnimLibrary::stand() const { return get("3p_stand"); }
const bf2::AnimationClip* SoldierAnimLibrary::walk_forward() const { return get("3p_walkforward"); }
const bf2::AnimationClip* SoldierAnimLibrary::run_forward() const { return get("3p_runforward"); }
const bf2::AnimationClip* SoldierAnimLibrary::crouch_still() const { return get("3p_crouchstill"); }
const bf2::AnimationClip* SoldierAnimLibrary::fire() const { return get("3p_fire"); }
const bf2::AnimationClip* SoldierAnimLibrary::die() const { return get("3p_die_1"); }

void SoldierAnimLibrary::fill_anim_set(SoldierAnimSet& out) const {
  out.stand = stand();
  out.walk = walk_forward();
  out.run = run_forward();
  out.crouch = crouch_still();
  out.fire = fire();
  out.death = die();
  if (!out.death) out.death = get("3p_die_2");
  out.prone = get("3p_pronestill");
  out.reload = get("3p_reload");
}

}  // namespace dalian
