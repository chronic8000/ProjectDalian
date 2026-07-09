#include "game_audio.hpp"

#include "audio_context.hpp"

#include "engine/core/resource_manager.hpp"

#include <SDL_mixer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace dalian {
namespace {

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string strip_quotes(std::string s) {
  while (!s.empty() && (s.front() == '"' || s.front() == '\'')) s.erase(s.begin());
  while (!s.empty() && (s.back() == '"' || s.back() == '\'')) s.pop_back();
  return s;
}

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_quote = false;
  for (char c : line) {
    if (c == '"') {
      in_quote = !in_quote;
      cur.push_back(c);
    } else if (std::isspace(static_cast<unsigned char>(c)) && !in_quote) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string extract_quoted_path(const std::string& line) {
  const auto a = line.find('"');
  if (a == std::string::npos) return {};
  const auto b = line.find('"', a + 1);
  if (b == std::string::npos) return {};
  return line.substr(a + 1, b - a - 1);
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

std::vector<VoiceBank::Clip>* role_bucket(VoiceBank::Entry& e, const std::string& role) {
  if (role == "squadleader") return &e.squadleader;
  if (role == "commander") return &e.commander;
  return &e.grunt;
}

}  // namespace

std::string resolve_bf2_sound_path(std::string path, const std::string& language) {
  path = lower_copy(path);
  for (char& c : path)
    if (c == '\\') c = '/';
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  if (path.rfind("common/", 0) == 0) path = path.substr(7);
  if (path.rfind("objects/", 0) == 0) path = path.substr(8);
  if (path.rfind("sound/", 0) != 0 && path.find('/') != std::string::npos &&
      (path.find("grunt/") == 0 || path.find("squadleader/") == 0 ||
       path.find("commander/") == 0)) {
    path = resolve_voice_path(path, language);
  }
  return path;
}

std::string resolve_voice_path(const std::string& relative, const std::string& language) {
  std::string rel = lower_copy(relative);
  for (char& c : rel)
    if (c == '\\') c = '/';
  return "sound/" + lower_copy(language) + "/" + rel;
}

bool VoiceBank::load_from_archives(bf2::ResourceManager& resources,
                                   const std::string& language) {
  messages_.clear();
  static const char* kFiles[] = {"sound/voicemessages_automaticallytriggered.con",
                                 "sound/voicemessages_playertriggered.con",
                                 "sound/voicemessages_help.con"};
  std::string combined;
  for (const char* f : kFiles) {
    if (const auto bytes = resources.read_bytes(f)) {
      combined.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
      combined.push_back('\n');
    }
  }
  if (combined.empty()) {
    std::cerr << "GameAudio: no VoiceMessages*.con found in Common archive\n";
    return false;
  }

  std::string current_id;
  for (const std::string& line : split_lines(combined)) {
    if (line.find("gamelogic.messages.addMessage") != std::string::npos) {
      const auto tokens = tokenize(line);
      if (tokens.size() >= 2) current_id = strip_quotes(tokens.back());
      continue;
    }
    if (current_id.empty() || line.find("gamelogic.messages.addRadioVoice") == std::string::npos)
      continue;
    const auto tokens = tokenize(line);
    if (tokens.size() < 3) continue;
    const std::string role = strip_quotes(tokens[1]);
    std::string radio;
    std::string local;
    for (std::size_t i = 2; i < tokens.size(); ++i) {
      const std::string t = strip_quotes(tokens[i]);
      if (t.empty()) continue;
      if (t.find(".ogg") == std::string::npos && t.find(".wav") == std::string::npos) continue;
      if (t.find('/') == std::string::npos) continue;
      if (radio.empty()) radio = t;
      else local = t;
    }
    if (radio.empty() && local.empty()) continue;
    VoiceBank::Clip clip{radio, local};
    auto& entry = messages_[current_id];
    role_bucket(entry, role)->push_back(clip);
  }
  std::cout << "GameAudio: loaded " << messages_.size() << " BF2 voice messages\n";
  (void)language;
  return !messages_.empty();
}

bool VoiceBank::play(bf2::ResourceManager& resources, GameAudio& audio,
                     const std::string& message_id, const std::string& role,
                     bool use_radio) const {
  const auto it = messages_.find(message_id);
  if (it == messages_.end()) return false;
  const VoiceBank::Entry& e = it->second;
  const std::vector<VoiceBank::Clip>* clips = &e.grunt;
  if (role == "squadleader" && !e.squadleader.empty()) clips = &e.squadleader;
  if (role == "commander" && !e.commander.empty()) clips = &e.commander;
  if (clips->empty()) return false;
  const VoiceBank::Clip& clip = (*clips)[static_cast<std::size_t>(std::rand()) % clips->size()];
  if (use_radio && !clip.radio.empty()) {
    audio.play_2d(resources, resolve_voice_path(clip.radio), 1.f);
    return true;
  }
  if (!clip.local.empty()) {
    audio.play_2d(resources, resolve_voice_path(clip.local), 1.f);
    return true;
  }
  if (!clip.radio.empty()) {
    audio.play_2d(resources, resolve_voice_path(clip.radio), 1.f);
    return true;
  }
  return false;
}

bool GameAudio::init(float master_volume, float sfx_volume) {
  if (!AudioContext::acquire()) return false;
  master_ = master_volume;
  sfx_ = sfx_volume;
  ready_ = true;
  return true;
}

void GameAudio::shutdown() {
  stop_ambient();
  free_cache();
  ready_ = false;
  AudioContext::release();
}

void GameAudio::set_volumes(float master_volume, float sfx_volume) {
  master_ = master_volume;
  sfx_ = sfx_volume;
  if (ambient_music_) Mix_VolumeMusic(volume_int(0.35f));
}

int GameAudio::volume_int(float scale) const {
  return static_cast<int>(std::clamp(scale * sfx_ * master_, 0.f, 1.f) * MIX_MAX_VOLUME);
}

void GameAudio::free_cache() {
  for (auto& [k, v] : chunk_cache_) {
    if (v.chunk) Mix_FreeChunk(static_cast<Mix_Chunk*>(v.chunk));
    (void)k;
  }
  chunk_cache_.clear();
}

void* GameAudio::load_chunk(bf2::ResourceManager& resources, const std::string& bf2_path) {
  const std::string key = resolve_bf2_sound_path(bf2_path);
  if (key.empty()) return nullptr;
  if (auto it = chunk_cache_.find(key); it != chunk_cache_.end()) return it->second.chunk;
  const auto bytes = resources.read_bytes(key);
  if (!bytes || bytes->empty()) return nullptr;
  SDL_RWops* rw = SDL_RWFromConstMem(bytes->data(), static_cast<int>(bytes->size()));
  if (!rw) return nullptr;
  Mix_Chunk* chunk = Mix_LoadWAV_RW(rw, 1);
  if (!chunk) return nullptr;
  chunk_cache_[key] = {chunk};
  return chunk;
}

void* GameAudio::load_music(bf2::ResourceManager& resources, const std::string& bf2_path) {
  const std::string key = resolve_bf2_sound_path(bf2_path);
  const auto bytes = resources.read_bytes(key);
  if (!bytes || bytes->empty()) return nullptr;
  SDL_RWops* rw = SDL_RWFromConstMem(bytes->data(), static_cast<int>(bytes->size()));
  if (!rw) return nullptr;
  return Mix_LoadMUS_RW(rw, 1);
}

void GameAudio::play_2d(bf2::ResourceManager& resources, const std::string& bf2_path,
                        float volume_scale) {
  if (!ready_) return;
  std::string key = bf2_path;
  if (key.rfind("sound/", 0) != 0) key = resolve_bf2_sound_path(bf2_path);
  if (auto it = chunk_cache_.find(key); it != chunk_cache_.end() && it->second.chunk) {
    const int ch = Mix_PlayChannel(-1, static_cast<Mix_Chunk*>(it->second.chunk), 0);
    if (ch >= 0) Mix_Volume(ch, volume_int(volume_scale));
    return;
  }
  const auto bytes = resources.read_bytes(key);
  if (!bytes || bytes->empty()) return;
  SDL_RWops* rw = SDL_RWFromConstMem(bytes->data(), static_cast<int>(bytes->size()));
  if (!rw) return;
  Mix_Chunk* chunk = Mix_LoadWAV_RW(rw, 1);
  if (!chunk) return;
  chunk_cache_[key] = {chunk};
  const int ch = Mix_PlayChannel(-1, chunk, 0);
  if (ch >= 0) Mix_Volume(ch, volume_int(volume_scale));
}

void GameAudio::play_3d(bf2::ResourceManager& resources, const std::string& bf2_path, float x,
                        float y, float z, float listener_x, float listener_y, float listener_z,
                        float volume_scale) {
  if (!ready_) return;
  void* c = load_chunk(resources, bf2_path);
  if (!c) return;
  const float dx = x - listener_x;
  const float dy = y - listener_y;
  const float dz = z - listener_z;
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  const float falloff = std::clamp(1.f - dist / 120.f, 0.05f, 1.f);
  const int ch = Mix_PlayChannel(-1, static_cast<Mix_Chunk*>(c), 0);
  if (ch >= 0) {
    Mix_Volume(ch, volume_int(volume_scale * falloff));
    const float pan = std::clamp(dx / 40.f, -1.f, 1.f);
    Mix_SetPanning(ch, static_cast<Uint8>((1.f - pan) * 127.f), static_cast<Uint8>((1.f + pan) * 127.f));
  }
}

bool GameAudio::load_weapon_sounds(bf2::ResourceManager& resources, const std::string& tweak_path,
                                   WeaponSoundSet& out) {
  const auto bytes = resources.read_bytes(tweak_path);
  if (!bytes) return false;
  const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  std::string pending;
  for (const std::string& line : split_lines(text)) {
    if (line.find("ObjectTemplate.create Sound") != std::string::npos ||
        line.find("ObjectTemplate.activeSafe Sound") != std::string::npos) {
      pending = line;
      continue;
    }
    if (line.find("ObjectTemplate.soundFilename") == std::string::npos) continue;
    const std::string path = extract_quoted_path(line);
    if (path.empty() || pending.empty()) continue;
    const std::string tag = lower_copy(pending);
    if (tag.find("fire1p") != std::string::npos) out.fire_1p = path;
    else if (tag.find("fire3p") != std::string::npos) out.fire_3p = path;
    else if (tag.find("reload1p") != std::string::npos) out.reload_1p = path;
    else if (tag.find("deploy1p") != std::string::npos) out.deploy_1p = path;
  }
  return !out.fire_1p.empty();
}

bool GameAudio::load_vehicle_sounds(bf2::ResourceManager& resources, const std::string& tweak_path,
                                    VehicleSoundSet& out) {
  const auto bytes = resources.read_bytes(tweak_path);
  if (!bytes) return false;
  const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  std::string pending;
  for (const std::string& line : split_lines(text)) {
    if (line.find("ObjectTemplate.create Sound") != std::string::npos ||
        line.find("ObjectTemplate.activeSafe Sound") != std::string::npos) {
      pending = line;
      continue;
    }
    if (line.find("ObjectTemplate.soundFilename") == std::string::npos) continue;
    const std::string path = extract_quoted_path(line);
    if (path.empty() || pending.empty()) continue;
    const std::string tag = lower_copy(pending);
    if (tag.find("enginestart") != std::string::npos || tag.find("engine_start") != std::string::npos)
      out.engine_start = path;
    else if (tag.find("enginestop") != std::string::npos ||
             tag.find("engine_stop") != std::string::npos)
      out.engine_stop = path;
    else if (tag.find("engine") != std::string::npos && out.engine_loop.empty())
      out.engine_loop = path;
    else if (tag.find("tire") != std::string::npos || tag.find("tyre") != std::string::npos ||
             tag.find("track") != std::string::npos)
      out.tire_roll = path;
  }
  (void)resources;
  return !out.engine_loop.empty() || !out.engine_start.empty();
}

int GameAudio::play_loop(bf2::ResourceManager& resources, const std::string& bf2_path,
                         float volume_scale) {
  if (!ready_ || bf2_path.empty()) return -1;
  void* c = load_chunk(resources, bf2_path);
  if (!c) return -1;
  const int ch = Mix_PlayChannel(-1, static_cast<Mix_Chunk*>(c), -1);
  if (ch >= 0) Mix_Volume(ch, volume_int(volume_scale));
  return ch;
}

void GameAudio::set_channel_volume(int channel, float volume_scale) {
  if (channel >= 0 && Mix_Playing(channel)) Mix_Volume(channel, volume_int(volume_scale));
}

void GameAudio::stop_channel(int channel) {
  if (channel >= 0) Mix_HaltChannel(channel);
}

bool GameAudio::channel_playing(int channel) const {
  return channel >= 0 && Mix_Playing(channel) != 0;
}

bool GameAudio::start_level_ambient(bf2::ResourceManager& resources,
                                    const std::string& ambient_con_bytes) {
  stop_ambient();
  std::string global_path;
  for (const std::string& line : split_lines(ambient_con_bytes)) {
    if (line.find("ObjectTemplate.soundFilename") == std::string::npos) continue;
    const std::string path = extract_quoted_path(line);
    if (path.empty()) continue;
    const std::string lp = lower_copy(path);
    if (lp.find("global_ambient") != std::string::npos ||
        lp.find("global_ambience") != std::string::npos) {
      global_path = path;
      break;
    }
    if (global_path.empty()) global_path = path;
  }
  if (global_path.empty()) return false;
  void* mus = load_music(resources, global_path);
  if (!mus) {
    std::cerr << "GameAudio: failed to load ambient " << global_path << '\n';
    return false;
  }
  ambient_music_ = mus;
  Mix_VolumeMusic(volume_int(0.35f));
  if (Mix_PlayMusic(static_cast<Mix_Music*>(ambient_music_), -1) != 0) {
    std::cerr << "GameAudio: ambient play failed: " << Mix_GetError() << '\n';
    Mix_FreeMusic(static_cast<Mix_Music*>(ambient_music_));
    ambient_music_ = nullptr;
    return false;
  }
  std::cout << "GameAudio: ambient loop " << global_path << '\n';
  return true;
}

void GameAudio::stop_ambient() {
  if (!ambient_music_) return;
  Mix_HaltMusic();
  Mix_FreeMusic(static_cast<Mix_Music*>(ambient_music_));
  ambient_music_ = nullptr;
}

void GameAudio::play_weapon_fire(bf2::ResourceManager& resources, bool first_person) {
  if (!weapon_) return;
  const std::string& path = first_person ? weapon_->fire_1p : weapon_->fire_3p;
  if (path.empty()) return;
  play_2d(resources, path, 0.85f);
}

void GameAudio::play_weapon_reload(bf2::ResourceManager& resources, bool first_person) {
  if (!weapon_) return;
  if (weapon_->reload_1p.empty()) return;
  play_2d(resources, weapon_->reload_1p, first_person ? 1.f : 0.7f);
}

void GameAudio::play_weapon_deploy(bf2::ResourceManager& resources, bool first_person) {
  if (!weapon_) return;
  if (weapon_->deploy_1p.empty()) return;
  play_2d(resources, weapon_->deploy_1p, first_person ? 1.f : 0.7f);
}

void GameAudio::play_voice(bf2::ResourceManager& resources, VoiceBank& bank,
                           const std::string& message_id, bool use_radio) {
  bank.play(resources, *this, message_id, "grunt", use_radio);
}

}  // namespace dalian
