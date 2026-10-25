#include "jc8012p4_storage_player.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cctype>

namespace esphome::jc8012p4_storage {

static const char *const TAG = "jc8012p4_storage.player";
static constexpr size_t ENCODED_BUFFER_SIZE = 32 * 1024;
static constexpr size_t DECODER_INPUT_CHUNK_SIZE = 4096;
static constexpr size_t DECODER_OUTPUT_BUFFER_SIZE = 12 * 1024;

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
  if (this->decoder_ == nullptr || this->state == media_player::MEDIA_PLAYER_STATE_PAUSED)
    return;

  this->feed_decoder_();
  const audio::AudioDecoderState decoder_state = this->decoder_->decode(this->file_eof_);

  if (!this->sink_ready_ && this->decoder_->get_audio_stream_info().has_value()) {
    const audio::AudioStreamInfo &info = this->decoder_->get_audio_stream_info().value();
    if (info.get_bits_per_sample() != 16 || info.get_channels() < 1 || info.get_channels() > 2) {
      ESP_LOGE(TAG, "Unsupported decoded audio: bits=%u, channels=%u", info.get_bits_per_sample(),
               info.get_channels());
      this->stop_(true);
      return;
    }
    this->speaker_->set_audio_stream_info(info);
    if (this->decoder_->add_sink(this->speaker_) != ESP_OK) {
      ESP_LOGE(TAG, "Could not connect the decoder to the speaker");
      this->stop_(true);
      return;
    }
    this->speaker_->start();
    this->sink_ready_ = true;
    ESP_LOGI(TAG, "Audio stream: %u-bit, %u channel(s), %" PRIu32 " Hz", info.get_bits_per_sample(),
             info.get_channels(), info.get_sample_rate());
  }

  if (decoder_state == audio::AudioDecoderState::FAILED) {
    ESP_LOGE(TAG, "Audio decoding failed");
    this->stop_(true);
  } else if (decoder_state == audio::AudioDecoderState::FINISHED) {
    this->finish_playback_();
  }
}

void Jc8012p4StoragePlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "SD card media player:");
  ESP_LOGCONFIG(TAG, "  Formats: MP3 and WAV");
}

void Jc8012p4StoragePlayer::on_shutdown() { this->stop_(true); }

media_player::MediaPlayerTraits Jc8012p4StoragePlayer::get_traits() {
  media_player::MediaPlayerTraits traits;
  traits.add_feature_flags(media_player::MediaPlayerEntityFeature::PAUSE);
  traits.clear_feature_flags(media_player::MediaPlayerEntityFeature::BROWSE_MEDIA);
  for (const auto purpose : {media_player::MediaPlayerFormatPurpose::PURPOSE_DEFAULT,
                             media_player::MediaPlayerFormatPurpose::PURPOSE_ANNOUNCEMENT}) {
    traits.get_supported_formats().push_back({"mp3", 0, 0, purpose, 0});
    traits.get_supported_formats().push_back({"wav", 0, 0, purpose, 2});
  }
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
      if (this->decoder_ != nullptr) {
        this->decoder_->set_pause_output_state(true);
        this->speaker_->set_pause_state(true);
        this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        this->publish_state();
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PLAY:
      if (this->decoder_ != nullptr && this->state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
        this->decoder_->set_pause_output_state(false);
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
        this->decoder_->set_pause_output_state(false);
        this->speaker_->set_pause_state(false);
        this->state = this->announcement_ ? media_player::MEDIA_PLAYER_STATE_ANNOUNCING
                                         : media_player::MEDIA_PLAYER_STATE_PLAYING;
      } else if (this->decoder_ != nullptr) {
        this->decoder_->set_pause_output_state(true);
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
  const audio::AudioFileType file_type = this->detect_file_type_(uri);
  if (file_type == audio::AudioFileType::NONE) {
    ESP_LOGE(TAG, "Unsupported file type: %s", uri.c_str());
    return false;
  }

  this->file_ = this->storage_->open(uri);
  if (this->file_ == nullptr) {
    ESP_LOGE(TAG, "Cannot open audio file: %s", uri.c_str());
    return false;
  }

  auto ring_buffer = ring_buffer::RingBuffer::create(ENCODED_BUFFER_SIZE);
  if (ring_buffer == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the encoded-audio buffer");
    fclose(this->file_);
    this->file_ = nullptr;
    return false;
  }
  this->encoded_buffer_ = std::shared_ptr<ring_buffer::RingBuffer>(std::move(ring_buffer));
  this->decoder_ =
      std::make_unique<audio::AudioDecoder>(DECODER_INPUT_CHUNK_SIZE, DECODER_OUTPUT_BUFFER_SIZE);
  std::weak_ptr<ring_buffer::RingBuffer> encoded_buffer = this->encoded_buffer_;
  esp_err_t err = this->decoder_->start(file_type);
  if (err == ESP_OK)
    err = this->decoder_->add_source(encoded_buffer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not start the audio decoder: %s", esp_err_to_name(err));
    this->stop_(true);
    return false;
  }

  this->file_eof_ = false;
  this->sink_ready_ = false;
  this->announcement_ = announcement;
  this->speaker_->set_pause_state(false);
  this->state = announcement ? media_player::MEDIA_PLAYER_STATE_ANNOUNCING
                             : media_player::MEDIA_PLAYER_STATE_PLAYING;
  this->publish_state();
  ESP_LOGI(TAG, "Playing %s", uri.c_str());
  return true;
}

void Jc8012p4StoragePlayer::feed_decoder_() {
  if (this->file_eof_ || this->file_ == nullptr || this->encoded_buffer_ == nullptr)
    return;
  const size_t wanted = std::min(this->file_buffer_.size(), this->encoded_buffer_->free());
  if (wanted == 0)
    return;

  const size_t read = fread(this->file_buffer_.data(), 1, wanted, this->file_);
  if (read > 0)
    this->encoded_buffer_->write_without_replacement(this->file_buffer_.data(), read);
  if (read < wanted) {
    if (ferror(this->file_))
      ESP_LOGW(TAG, "Read error near the end of the audio file");
    this->file_eof_ = true;
    fclose(this->file_);
    this->file_ = nullptr;
  }
}

void Jc8012p4StoragePlayer::finish_playback_() {
  if (this->file_ != nullptr) {
    fclose(this->file_);
    this->file_ = nullptr;
  }
  this->decoder_.reset();
  this->encoded_buffer_.reset();
  this->sink_ready_ = false;
  this->speaker_->finish();
  this->draining_ = true;
}

void Jc8012p4StoragePlayer::stop_(bool immediate) {
  if (this->file_ != nullptr) {
    fclose(this->file_);
    this->file_ = nullptr;
  }
  this->decoder_.reset();
  this->encoded_buffer_.reset();
  this->file_eof_ = false;
  this->sink_ready_ = false;
  if (immediate) {
    this->speaker_->stop();
    this->draining_ = false;
    if (this->state != media_player::MEDIA_PLAYER_STATE_IDLE) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      this->publish_state();
    }
  }
}

audio::AudioFileType Jc8012p4StoragePlayer::detect_file_type_(const std::string &uri) const {
  std::string lower = uri;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower.ends_with(".mp3"))
    return audio::AudioFileType::MP3;
  if (lower.ends_with(".wav"))
    return audio::AudioFileType::WAV;
  return audio::AudioFileType::NONE;
}

}  // namespace esphome::jc8012p4_storage
