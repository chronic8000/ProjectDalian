#pragma once

// Ref-counted SDL_mixer init so menu music and in-game SFX share one device.

namespace dalian {

class AudioContext {
 public:
  static bool acquire();
  static void release();
  static bool ready();

 private:
  static int refs_;
  static bool open_;
};

}  // namespace dalian
