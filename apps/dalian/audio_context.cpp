#include "audio_context.hpp"

#include <SDL_mixer.h>

#include <iostream>

namespace dalian {

int AudioContext::refs_ = 0;
bool AudioContext::open_ = false;

bool AudioContext::acquire() {
  if (refs_ == 0) {
    if (Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3) == 0) {
      std::cerr << "AudioContext: Mix_Init failed: " << Mix_GetError() << '\n';
      return false;
    }
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) != 0) {
      std::cerr << "AudioContext: Mix_OpenAudio failed: " << Mix_GetError() << '\n';
      Mix_Quit();
      return false;
    }
    Mix_AllocateChannels(32);
    open_ = true;
  }
  ++refs_;
  return true;
}

void AudioContext::release() {
  if (refs_ <= 0) return;
  if (--refs_ == 0 && open_) {
    Mix_CloseAudio();
    Mix_Quit();
    open_ = false;
  }
}

bool AudioContext::ready() { return open_ && refs_ > 0; }

}  // namespace dalian
