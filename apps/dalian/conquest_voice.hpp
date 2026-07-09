#pragma once

#include "game_audio.hpp"

#include "engine/core/resource_manager.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace dalian {

// Semantic cues emitted by GameSim — resolved to real BF2 VoiceMessages IDs at play time.
inline const std::vector<const char*>& voice_ids_for_cue(std::string_view cue) {
  static const std::vector<const char*> kFlagCaptured = {
      "AUTO_MOODGP_flagcaptured", "AUTO_MOODGP_wehavecaptured", "flagcaptured",
      "AUTO_MOODGP_capturingflagcomplete"};
  static const std::vector<const char*> kFlagLost = {
      "AUTO_MOODGP_enemyflagcaptured", "AUTO_MOODGP_flagislost", "enemyflagcaptured",
      "AUTO_MOODGP_enemycapturingflag"};
  static const std::vector<const char*> kTicketsLow = {"AUTO_MOODGP_lowontickets",
                                                      "AUTO_MOODGP_ticketslow", "lowontickets"};
  static const std::vector<const char*> kVictory = {"AUTO_MOODGP_victory", "AUTO_MOODGP_wehavewon",
                                                    "victory"};
  static const std::vector<const char*> kDefeat = {"AUTO_MOODGP_defeat", "AUTO_MOODGP_wehavelost",
                                                  "defeat"};

  if (cue == "flag_captured_friendly") return kFlagCaptured;
  if (cue == "flag_lost_friendly") return kFlagLost;
  if (cue == "tickets_low_friendly") return kTicketsLow;
  if (cue == "victory_friendly") return kVictory;
  if (cue == "defeat_friendly") return kDefeat;
  static const std::vector<const char*> kEmpty;
  return kEmpty;
}

inline bool play_conquest_voice(bf2::ResourceManager& resources, GameAudio& audio,
                                VoiceBank& bank, std::string_view cue, bool use_radio = true) {
  for (const char* id : voice_ids_for_cue(cue)) {
    if (bank.messages().find(id) != bank.messages().end()) {
      audio.play_voice(resources, bank, id, use_radio);
      return true;
    }
  }
  return false;
}

}  // namespace dalian
