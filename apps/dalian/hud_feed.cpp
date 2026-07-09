#include "hud_feed.hpp"

#include "ui_layout.hpp"

#include <algorithm>
#include <cstdio>

namespace dalian {

void HudFeed::push(std::string text, float duration, HudFeedLine::Kind kind) {
  if (text.empty()) return;
  lines_.push_back({std::move(text), 0.f, duration, kind});
  if (lines_.size() > 8) lines_.erase(lines_.begin());
}

void HudFeed::push_kill(const std::string& killer, const std::string& victim,
                        const std::string& weapon) {
  char buf[256];
  const char* w = weapon.empty() ? "Unknown" : weapon.c_str();
  std::snprintf(buf, sizeof(buf), "%s killed %s with %s", killer.c_str(), victim.c_str(), w);
  push(buf, 9.f, HudFeedLine::Kind::Kill);
}

void HudFeed::tick(float dt) {
  for (auto& line : lines_) line.age += dt;
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const HudFeedLine& l) { return l.age >= l.duration; }),
                lines_.end());
}

void HudFeed::draw(bf2::Renderer& renderer, float design_w, float design_h) const {
  if (lines_.empty()) return;
  float y = 24.f;
  const float right = design_w - 24.f;
  for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
    const float fade =
        std::clamp(1.f - (it->age - (it->duration - 1.2f)) / 1.2f, 0.f, 1.f);
    if (fade <= 0.01f) continue;
    float r = 0.88f, g = 0.90f, b = 0.93f;
    if (it->kind == HudFeedLine::Kind::Join) {
      r = 0.98f;
      g = 0.78f;
      b = 0.12f;
    } else if (it->kind == HudFeedLine::Kind::Leave) {
      r = 0.98f;
      g = 0.42f;
      b = 0.28f;
    } else if (it->kind == HudFeedLine::Kind::Kill) {
      r = 0.92f;
      g = 0.94f;
      b = 0.96f;
    }
    const float scale = 1.25f;
    const float max_w = design_w - 48.f;
    const std::string clipped = truncate_text(renderer, it->text, scale, max_w);
    const float tw = renderer.ui_text_width(clipped.c_str(), scale);
    const float x = std::max(24.f, right - tw);
    renderer.ui_rect(x - 10.f, y - 4.f, tw + 20.f, 24.f, 0.04f, 0.05f, 0.07f, 0.72f * fade);
    renderer.ui_text(x, y, scale, clipped.c_str(), r, g, b, fade);
    y += 28.f;
    if (y > design_h * 0.45f) break;
  }
}

}  // namespace dalian
