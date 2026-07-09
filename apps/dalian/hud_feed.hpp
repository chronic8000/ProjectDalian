#pragma once

#include "engine/render/renderer.hpp"

#include <string>
#include <vector>

namespace dalian {

struct HudFeedLine {
  std::string text;
  float age = 0.f;
  float duration = 8.f;
  enum class Kind { Join, Leave, Kill, Info } kind = Kind::Info;
};

class HudFeed {
 public:
  void push(std::string text, float duration = 8.f, HudFeedLine::Kind kind = HudFeedLine::Kind::Info);
  void push_kill(const std::string& killer, const std::string& victim, const std::string& weapon);
  void tick(float dt);
  void draw(bf2::Renderer& renderer, float design_w, float design_h) const;

 private:
  std::vector<HudFeedLine> lines_;
};

}  // namespace dalian
