#include "jc8012p4_storage.h"

#include "esphome/core/log.h"

#include <cmath>

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome::jc8012p4_storage {

static const char *const TAG = "jc8012p4_storage";
static constexpr const char *MOUNT_POINT = "/sdcard";
static constexpr uint32_t CHECK_INTERVAL_MS = 30000;

void Jc8012p4Storage::setup() { this->last_check_ = millis(); }

void Jc8012p4Storage::on_ready() {
  // Tessera runs plugin callbacks on core 1. On ESP32-P4, initializing the
  // SDMMC host from that callback can fault inside the IDF driver. The mount
  // task is pinned to core 0, as ESP-IDF's app_main is in the stock example.
  this->ready_ = true;
  this->start_mount_();
}

void Jc8012p4Storage::loop() {
  if (!this->ready_)
    return;

  if (this->mount_finished_.exchange(false)) {
    this->finish_mount_();
    this->last_check_ = millis();
  }

  const uint32_t now = millis();
  if (now - this->last_check_ < CHECK_INTERVAL_MS)
    return;
  this->last_check_ = now;
  if (!this->mounted_)
    this->start_mount_();
  else
    this->publish_stats_();
}

void Jc8012p4Storage::dump_config() {
  ESP_LOGCONFIG(TAG, "JC8012P4 SD card:");
  ESP_LOGCONFIG(TAG, "  Mount point: %s", MOUNT_POINT);
  ESP_LOGCONFIG(TAG, "  Interface: SDMMC 4-bit");
  ESP_LOGCONFIG(TAG, "  Pins: CLK 43, CMD 44, D0 39, D1 40, D2 41, D3 42");
  ESP_LOGCONFIG(TAG, "  Mounted: %s", YESNO(this->mounted_));
  LOG_BINARY_SENSOR("  ", "Card present", this->card_present_sensor_);
  LOG_SENSOR("  ", "Total space", this->total_space_sensor_);
  LOG_SENSOR("  ", "Free space", this->free_space_sensor_);
  LOG_SENSOR("  ", "Used", this->used_percent_sensor_);
}

void Jc8012p4Storage::on_shutdown() {
  if (!this->mounting_.load())
    this->unmount_();
}

void Jc8012p4Storage::start_mount_() {
  bool expected = false;
  if (!this->mounting_.compare_exchange_strong(expected, true))
    return;

  this->mount_finished_.store(false);
  const BaseType_t created =
      xTaskCreatePinnedToCore(mount_task_, "jc_sd_mount", 12288, this, 1, nullptr, 0);
  if (created != pdPASS) {
    this->mounting_.store(false);
    ESP_LOGE(TAG, "Could not create SD mount task");
  }
}

void Jc8012p4Storage::mount_task_(void *parameter) {
  auto *self = static_cast<Jc8012p4Storage *>(parameter);
  self->mount_error_.store(self->mount_card_());
  self->mounting_.store(false);
  self->mount_finished_.store(true);
  vTaskDelete(nullptr);
}

esp_err_t Jc8012p4Storage::mount_card_() {
  esp_vfs_fat_sdmmc_mount_config_t mount_config{};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 5;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 4;
  slot_config.clk = GPIO_NUM_43;
  slot_config.cmd = GPIO_NUM_44;
  slot_config.d0 = GPIO_NUM_39;
  slot_config.d1 = GPIO_NUM_40;
  slot_config.d2 = GPIO_NUM_41;
  slot_config.d3 = GPIO_NUM_42;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &this->card_);
}

void Jc8012p4Storage::finish_mount_() {
  const esp_err_t err = static_cast<esp_err_t>(this->mount_error_.load());
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(err));
    this->mounted_ = false;
    this->card_ = nullptr;
    if (this->card_present_sensor_ != nullptr)
      this->card_present_sensor_->publish_state(false);
    return;
  }

  this->mounted_ = true;
  if (this->card_present_sensor_ != nullptr)
    this->card_present_sensor_->publish_state(true);
  ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
  this->publish_stats_();
}

void Jc8012p4Storage::unmount_() {
  if (!this->mounted_)
    return;
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, this->card_);
  this->card_ = nullptr;
  this->mounted_ = false;
  if (this->card_present_sensor_ != nullptr)
    this->card_present_sensor_->publish_state(false);
}

void Jc8012p4Storage::publish_stats_() {
  uint64_t total = 0;
  uint64_t free = 0;
  if (esp_vfs_fat_info(MOUNT_POINT, &total, &free) != ESP_OK) {
    ESP_LOGW(TAG, "SD card is no longer readable");
    this->unmount_();
    return;
  }

  constexpr double BYTES_PER_GB = 1000000000.0;

  if (this->total_space_sensor_ != nullptr)
    this->total_space_sensor_->publish_state(static_cast<double>(total) / BYTES_PER_GB);
  if (this->free_space_sensor_ != nullptr)
    this->free_space_sensor_->publish_state(static_cast<double>(free) / BYTES_PER_GB);
  if (this->used_percent_sensor_ != nullptr) {
    const float used = total == 0 ? NAN : 100.0f * static_cast<float>(total - free) / static_cast<float>(total);
    this->used_percent_sensor_->publish_state(used);
  }
}

bool Jc8012p4Storage::make_path_(const std::string &path, std::string &absolute) const {
  if (path.empty() || path.find("..") != std::string::npos)
    return false;

  if (path.rfind("sd://", 0) == 0)
    absolute = std::string(MOUNT_POINT) + "/" + path.substr(5);
  else if (path.rfind("file:///sdcard/", 0) == 0)
    absolute = path.substr(7);
  else if (path == MOUNT_POINT || path.rfind("/sdcard/", 0) == 0)
    absolute = path;
  else
    return false;
  return absolute.size() > sizeof(MOUNT_POINT);
}

FILE *Jc8012p4Storage::open(const std::string &path, const char *mode) {
  if (!this->mounted_)
    return nullptr;
  std::string absolute;
  if (!this->make_path_(path, absolute)) {
    ESP_LOGW(TAG, "Rejected SD path: %s", path.c_str());
    return nullptr;
  }
  return fopen(absolute.c_str(), mode);
}

}  // namespace esphome::jc8012p4_storage
