#pragma once

#include "esphome/components/audio/audio.h"
#include "esphome/components/jc8012p4_storage/jc8012p4_storage.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include <array>
#include <cstdio>
#include <string>

namespace esphome::jc8012p4_storage {

class Jc8012p4StoragePlayer : public Component, public media_player::MediaPlayer {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  void set_storage(Jc8012p4Storage *storage) { this->storage_ = storage; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  media_player::MediaPlayerTraits get_traits() override;
  bool is_muted() const override { return this->muted_; }

 protected:
  void control(const media_player::MediaPlayerCall &call) override;
  bool start_file_(const std::string &uri, bool announcement);
  bool parse_wav_();
  void stop_(bool immediate);
  static uint16_t read_u16_(const uint8_t *data);
  static uint32_t read_u32_(const uint8_t *data);

  Jc8012p4Storage *storage_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  FILE *file_{nullptr};
  uint32_t bytes_remaining_{0};
  std::array<uint8_t, 4096> buffer_{};
  size_t buffer_length_{0};
  size_t buffer_position_{0};
  bool draining_{false};
  bool muted_{false};
  bool announcement_{false};
};

}  // namespace esphome::jc8012p4_storage
