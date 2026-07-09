#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {
class ResourceManager;
}

namespace dalian {

struct VoiceBank {
  struct Clip {
    std::string radio;
    std::string local;
  };
  struct Entry {
    std::vector<Clip> grunt;
    std::vector<Clip> squadleader;
    std::vector<Clip> commander;
  };

  bool load_from_archives(bf2::ResourceManager& resources, const std::string& language = "english");
  bool play(bf2::ResourceManager& resources, class GameAudio& audio, const std::string& message_id,
            const std::string& role = "grunt", bool use_radio = false) const;

  const std::unordered_map<std::string, Entry>& messages() const { return messages_; }

 private:
  std::unordered_map<std::string, Entry> messages_;
};

struct WeaponSoundSet {
  std::string fire_1p;
  std::string fire_3p;
  std::string reload_1p;
  std::string deploy_1p;
};

struct VehicleSoundSet {
  std::string engine_loop;
  std::string engine_start;
  std::string engine_stop;
  std::string tire_roll;
};

class GameAudio {
 public:
  bool init(float master_volume, float sfx_volume);
  void shutdown();
  void set_volumes(float master_volume, float sfx_volume);

  bool load_weapon_sounds(bf2::ResourceManager& resources, const std::string& tweak_path,
                          WeaponSoundSet& out);
  bool load_vehicle_sounds(bf2::ResourceManager& resources, const std::string& tweak_path,
                           VehicleSoundSet& out);
  bool start_level_ambient(bf2::ResourceManager& resources, const std::string& ambient_con_bytes);
  void stop_ambient();

  void set_weapon_sounds(const WeaponSoundSet* sounds) { weapon_ = sounds; }

  void play_2d(bf2::ResourceManager& resources, const std::string& bf2_path, float volume_scale = 1.f);
  void play_3d(bf2::ResourceManager& resources, const std::string& bf2_path, float x, float y,
               float z, float listener_x, float listener_y, float listener_z,
               float volume_scale = 1.f);

  int play_loop(bf2::ResourceManager& resources, const std::string& bf2_path, float volume_scale = 1.f);
  void set_channel_volume(int channel, float volume_scale);
  void stop_channel(int channel);
  bool channel_playing(int channel) const;

  void play_weapon_fire(bf2::ResourceManager& resources, bool first_person);
  void play_weapon_reload(bf2::ResourceManager& resources, bool first_person);
  void play_weapon_deploy(bf2::ResourceManager& resources, bool first_person);

  void play_voice(bf2::ResourceManager& resources, VoiceBank& bank, const std::string& message_id,
                  bool use_radio = false);

 private:
  struct CachedChunk {
    void* chunk = nullptr;
  };

  bool ready_ = false;
  float master_ = 1.f;
  float sfx_ = 1.f;
  const WeaponSoundSet* weapon_ = nullptr;
  void* ambient_music_ = nullptr;
  std::unordered_map<std::string, CachedChunk> chunk_cache_;

  void* load_chunk(bf2::ResourceManager& resources, const std::string& bf2_path);
  void* load_music(bf2::ResourceManager& resources, const std::string& bf2_path);
  int volume_int(float scale) const;
  void free_cache();
};

std::string resolve_bf2_sound_path(std::string path, const std::string& language = "english");
std::string resolve_voice_path(const std::string& relative, const std::string& language = "english");

}  // namespace dalian
