#include "jc8012p4_storage.h"

#include "esphome/core/log.h"

#include <cmath>

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"

namespace esphome::jc8012p4_storage {

static const char *const TAG = "jc8012p4_storage";
static constexpr const char *MOUNT_POINT = "/sdcard";
static constexpr uint32_t CHECK_INTERVAL_MS = 30000;

void Jc8012p4Storage::setup() {
  this->mount_();
  this->last_check_ = millis();
}

void Jc8012p4Storage::loop() {
  const uint32_t now = millis();
  if (now - this->last_check_ < CHECK_INTERVAL_MS)
    return;
  this->last_check_ = now;
  if (!this->mounted_)
    this->mount_();
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

void Jc8012p4Storage::on_shutdown() { this->unmount_(); }

bool Jc8012p4Storage::mount_() {
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

  const esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(err));
    this->mounted_ = false;
    this->card_ = nullptr;
    if (this->card_present_sensor_ != nullptr)
      this->card_present_sensor_->publish_state(false);
    return false;
  }

  this->mounted_ = true;
  if (this->card_present_sensor_ != nullptr)
    this->card_present_sensor_->publish_state(true);
  ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
  this->publish_stats_();
  return true;
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
