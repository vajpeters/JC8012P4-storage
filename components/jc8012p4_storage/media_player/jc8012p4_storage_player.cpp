#include "jc8012p4_storage_player.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::jc8012p4_storage {

static const char *const TAG = "jc8012p4_storage.player";

void Jc8012p4StoragePlayer::setup() {
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
  this->volume = this->speaker_->get_volume();
  this->muted_ = this->speaker_->get_mute_state();
  this->publish_state();
}

void Jc8012p4StoragePlayer::loop() {
  if (this->draining_) {
    if (this->speaker_->is_stopped() || !this->speaker_->has_buffered_data()) {
      this->draining_ = false;
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      this->publish_state();
    }
    return;
  }
  if (this->file_ == nullptr || this->state == media_player::MEDIA_PLAYER_STATE_PAUSED)
    return;

  if (this->buffer_position_ == this->buffer_length_) {
    if (this->bytes_remaining_ == 0) {
      this->stop_(false);
      return;
    }
    const size_t wanted = std::min<size_t>(this->buffer_.size(), this->bytes_remaining_);
    this->buffer_length_ = fread(this->buffer_.data(), 1, wanted, this->file_);
    this->buffer_position_ = 0;
    this->bytes_remaining_ -= this->buffer_length_;
    if (this->buffer_length_ == 0) {
      ESP_LOGW(TAG, "Unexpected end of WAV file");
      this->stop_(false);
      return;
    }
  }

  const size_t written = this->speaker_->play(this->buffer_.data() + this->buffer_position_,
                                               this->buffer_length_ - this->buffer_position_, 0);
  this->buffer_position_ += written;
}

void Jc8012p4StoragePlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "SD card WAV player:");
  ESP_LOGCONFIG(TAG, "  Formats: PCM WAV, 16-bit, mono or stereo");
}

void Jc8012p4StoragePlayer::on_shutdown() { this->stop_(true); }

media_player::MediaPlayerTraits Jc8012p4StoragePlayer::get_traits() {
  media_player::MediaPlayerTraits traits;
  traits.add_feature_flags(media_player::MediaPlayerEntityFeature::PAUSE);
  traits.clear_feature_flags(media_player::MediaPlayerEntityFeature::BROWSE_MEDIA);
  traits.get_supported_formats().push_back(
      {"wav", 0, 0, media_player::MediaPlayerFormatPurpose::PURPOSE_DEFAULT, 2});
  traits.get_supported_formats().push_back(
      {"wav", 0, 0, media_player::MediaPlayerFormatPurpose::PURPOSE_ANNOUNCEMENT, 2});
  return traits;
}

void Jc8012p4StoragePlayer::control(const media_player::MediaPlayerCall &call) {
  if (call.get_volume().has_value()) {
    this->volume = call.get_volume().value();
    this->speaker_->set_volume(this->volume);
    this->publish_state();
  }

  if (call.get_media_url().has_value()) {
    const bool announcement = call.get_announcement().value_or(false);
    this->start_file_(call.get_media_url().value(), announcement);
  }

  if (!call.get_command().has_value())
    return;
  switch (call.get_command().value()) {
    case media_player::MEDIA_PLAYER_COMMAND_STOP:
      this->stop_(true);
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
      if (this->file_ != nullptr) {
        this->speaker_->set_pause_state(true);
        this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        this->publish_state();
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PLAY:
      if (this->file_ != nullptr && this->state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
        this->speaker_->set_pause_state(false);
        this->state = this->announcement_ ? media_player::MEDIA_PLAYER_STATE_ANNOUNCING
                                         : media_player::MEDIA_PLAYER_STATE_PLAYING;
        this->publish_state();
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_MUTE:
      this->muted_ = true;
      this->speaker_->set_mute_state(true);
      this->publish_state();
      break;
    case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
      this->muted_ = false;
      this->speaker_->set_mute_state(false);
      this->publish_state();
      break;
    case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
      if (this->state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
        this->speaker_->set_pause_state(false);
        this->state = this->announcement_ ? media_player::MEDIA_PLAYER_STATE_ANNOUNCING
                                         : media_player::MEDIA_PLAYER_STATE_PLAYING;
      } else if (this->file_ != nullptr) {
        this->speaker_->set_pause_state(true);
        this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
      }
      this->publish_state();
      break;
    default:
      break;
  }
}

bool Jc8012p4StoragePlayer::start_file_(const std::string &uri, bool announcement) {
  this->stop_(true);
  this->file_ = this->storage_->open(uri);
  if (this->file_ == nullptr) {
    ESP_LOGE(TAG, "Cannot open WAV file: %s", uri.c_str());
    return false;
  }
  if (!this->parse_wav_()) {
    fclose(this->file_);
    this->file_ = nullptr;
    return false;
  }

  this->buffer_length_ = 0;
  this->buffer_position_ = 0;
  this->announcement_ = announcement;
  this->speaker_->set_pause_state(false);
  this->speaker_->start();
  this->state = announcement ? media_player::MEDIA_PLAYER_STATE_ANNOUNCING
                             : media_player::MEDIA_PLAYER_STATE_PLAYING;
  this->publish_state();
  ESP_LOGI(TAG, "Playing %s", uri.c_str());
  return true;
}

bool Jc8012p4StoragePlayer::parse_wav_() {
  uint8_t header[12];
  if (fread(header, 1, sizeof(header), this->file_) != sizeof(header) || memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVE", 4) != 0) {
    ESP_LOGE(TAG, "File is not a RIFF/WAVE file");
    return false;
  }

  bool have_format = false;
  while (true) {
    uint8_t chunk[8];
    if (fread(chunk, 1, sizeof(chunk), this->file_) != sizeof(chunk))
      break;
    const uint32_t size = read_u32_(chunk + 4);
    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t format[16];
      if (size < sizeof(format) || fread(format, 1, sizeof(format), this->file_) != sizeof(format))
        break;
      const uint16_t codec = read_u16_(format);
      const uint16_t channels = read_u16_(format + 2);
      const uint32_t sample_rate = read_u32_(format + 4);
      const uint16_t bits = read_u16_(format + 14);
      if (codec != 1 || bits != 16 || channels < 1 || channels > 2 || sample_rate < 8000 || sample_rate > 96000) {
        ESP_LOGE(TAG, "Unsupported WAV: codec=%u, bits=%u, channels=%u, rate=%" PRIu32, codec, bits, channels,
                 sample_rate);
        return false;
      }
      this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(bits, channels, sample_rate));
      const long skip = static_cast<long>(size - sizeof(format) + (size & 1U));
      if (skip > 0)
        fseek(this->file_, skip, SEEK_CUR);
      have_format = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      if (!have_format) {
        ESP_LOGE(TAG, "WAV data appears before its format chunk");
        return false;
      }
      this->bytes_remaining_ = size;
      return size > 0;
    } else {
      fseek(this->file_, static_cast<long>(size + (size & 1U)), SEEK_CUR);
    }
  }
  ESP_LOGE(TAG, "Incomplete WAV file");
  return false;
}

void Jc8012p4StoragePlayer::stop_(bool immediate) {
  if (this->file_ != nullptr) {
    fclose(this->file_);
    this->file_ = nullptr;
  }
  this->buffer_length_ = 0;
  this->buffer_position_ = 0;
  this->bytes_remaining_ = 0;
  if (immediate) {
    this->speaker_->stop();
    this->draining_ = false;
    if (this->state != media_player::MEDIA_PLAYER_STATE_IDLE) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      this->publish_state();
    }
  } else {
    this->speaker_->finish();
    this->draining_ = true;
  }
}

uint16_t Jc8012p4StoragePlayer::read_u16_(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8;
}

uint32_t Jc8012p4StoragePlayer::read_u32_(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
         static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
}

}  // namespace esphome::jc8012p4_storage
