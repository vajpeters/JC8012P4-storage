#pragma once

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/jc8012p4_storage/jc8012p4_storage.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include <array>
#include <cstdio>
#include <memory>
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
  void feed_decoder_();
  void finish_playback_();
  void stop_(bool immediate);
  audio::AudioFileType detect_file_type_(const std::string &uri) const;

  Jc8012p4Storage *storage_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  FILE *file_{nullptr};
  std::shared_ptr<ring_buffer::RingBuffer> encoded_buffer_;
  std::unique_ptr<audio::AudioDecoder> decoder_;
  std::array<uint8_t, 4096> file_buffer_{};
  bool file_eof_{false};
  bool sink_ready_{false};
  bool draining_{false};
  bool muted_{false};
  bool announcement_{false};
};

}  // namespace esphome::jc8012p4_storage
