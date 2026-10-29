#include "jc8012p4_storage.h"

#include "esphome/core/log.h"

#include <cmath>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome::jc8012p4_storage {

static const char *const TAG = "jc8012p4_storage";
static constexpr const char *MOUNT_POINT = "/sdcard";
static constexpr uint32_t CHECK_INTERVAL_MS = 30000;
static constexpr uint32_t INITIAL_PROBE_DELAY_MS = 15000;
static constexpr uint32_t SPI_INIT_CLOCK_KHZ = 400;
static constexpr int SD_LDO_CHANNEL = 4;
static constexpr int SD_LDO_VOLTAGE_MV = 3300;

void Jc8012p4Storage::setup() {
  this->last_check_ = millis();
  if (this->card_detected_sensor_ != nullptr)
    this->card_detected_sensor_->publish_state(false);
  if (this->card_present_sensor_ != nullptr)
    this->card_present_sensor_->publish_state(false);
}

void Jc8012p4Storage::on_ready() {
  // Tessera runs plugin callbacks on core 1. On ESP32-P4, initializing the
  // SDMMC host from that callback can fault inside the IDF driver. The mount
  // task is pinned to core 0, as ESP-IDF's app_main is in the stock example.
  this->ready_ = true;
  this->last_check_ = millis();
}

void Jc8012p4Storage::set_probe_enabled(bool enabled) {
  if (enabled == this->probe_enabled_)
    return;
  this->probe_enabled_ = enabled;
  this->last_check_ = millis();
  if (!enabled) {
    if (!this->mounting_.load())
      this->unmount_();
    return;
  }
  ESP_LOGI(TAG, "SD card probe enabled by Home Assistant");
}

void Jc8012p4Storage::loop() {
  if (!this->ready_ || !this->probe_enabled_)
    return;

  if (this->mount_finished_.exchange(false)) {
    this->finish_mount_();
    this->last_check_ = millis();
  }
  if (this->stats_finished_.exchange(false))
    this->publish_stats_();

  const uint32_t now = millis();
  const uint32_t interval = this->mounted_ ? CHECK_INTERVAL_MS : INITIAL_PROBE_DELAY_MS;
  if (now - this->last_check_ < interval)
    return;
  this->last_check_ = now;
  if (!this->mounted_)
    this->start_mount_();
  else
    this->start_stats_();
}

void Jc8012p4Storage::dump_config() {
  ESP_LOGCONFIG(TAG, "JC8012P4 SD card:");
  ESP_LOGCONFIG(TAG, "  Mount point: %s", MOUNT_POINT);
  ESP_LOGCONFIG(TAG, "  Interface: SD card SPI mode");
  ESP_LOGCONFIG(TAG, "  Pins: CLK 43, MOSI/CMD 44, MISO/D0 39, CS/D3 42");
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

  this->card_detected_.store(false);

  // The TF socket's VDD pin is wired to ESP_LDO_VO4.  Acquiring this
  // official ESP-IDF LDO channel enables its 3.3 V supply and keeps it on
  // while the card is mounted.
  esp_ldo_channel_config_t ldo_config{};
  ldo_config.chan_id = SD_LDO_CHANNEL;
  ldo_config.voltage_mv = SD_LDO_VOLTAGE_MV;
  esp_err_t err = esp_ldo_acquire_channel(&ldo_config, &this->ldo_channel_);
  if (err != ESP_OK) {
    this->ldo_channel_ = nullptr;
    return err;
  }
  vTaskDelay(pdMS_TO_TICKS(10));

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = SPI_INIT_CLOCK_KHZ;
  this->spi_host_slot_ = host.slot;

  spi_bus_config_t bus_config{};
  bus_config.mosi_io_num = GPIO_NUM_44;
  bus_config.miso_io_num = GPIO_NUM_39;
  bus_config.sclk_io_num = GPIO_NUM_43;
  bus_config.quadwp_io_num = GPIO_NUM_NC;
  bus_config.quadhd_io_num = GPIO_NUM_NC;
  bus_config.max_transfer_sz = 16 * 1024;

  err = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus_config, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    this->spi_host_slot_ = -1;
    esp_ldo_release_channel(this->ldo_channel_);
    this->ldo_channel_ = nullptr;
    return err;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = GPIO_NUM_42;
  slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

  err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &this->card_);
  this->card_detected_.store(err != ESP_ERR_TIMEOUT);
  if (err != ESP_OK) {
    spi_bus_free(static_cast<spi_host_device_t>(host.slot));
    this->spi_host_slot_ = -1;
    esp_ldo_release_channel(this->ldo_channel_);
    this->ldo_channel_ = nullptr;
  }
  return err;
}

void Jc8012p4Storage::start_stats_() {
  bool expected = false;
  if (!this->stats_running_.compare_exchange_strong(expected, true))
    return;
  this->stats_finished_.store(false);
  const BaseType_t created = xTaskCreatePinnedToCore(stats_task_, "jc_sd_stats", 4096, this, 1, nullptr, 0);
  if (created != pdPASS) {
    this->stats_running_.store(false);
    ESP_LOGE(TAG, "Could not create SD statistics task");
  }
}

void Jc8012p4Storage::stats_task_(void *parameter) {
  auto *self = static_cast<Jc8012p4Storage *>(parameter);
  uint64_t total = 0;
  uint64_t free = 0;
  self->stats_error_.store(esp_vfs_fat_info(MOUNT_POINT, &total, &free));
  self->stats_total_.store(total);
  self->stats_free_.store(free);
  self->stats_running_.store(false);
  self->stats_finished_.store(true);
  vTaskDelete(nullptr);
}

void Jc8012p4Storage::finish_mount_() {
  const esp_err_t err = static_cast<esp_err_t>(this->mount_error_.load());
  if (this->card_detected_sensor_ != nullptr)
    this->card_detected_sensor_->publish_state(this->card_detected_.load());
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
  ESP_LOGI(TAG, "SD filesystem mounted; volume stats deferred");
}

void Jc8012p4Storage::unmount_() {
  if (!this->mounted_)
    return;
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, this->card_);
  if (this->spi_host_slot_ >= 0) {
    spi_bus_free(static_cast<spi_host_device_t>(this->spi_host_slot_));
    this->spi_host_slot_ = -1;
  }
  if (this->ldo_channel_ != nullptr) {
    esp_ldo_release_channel(this->ldo_channel_);
    this->ldo_channel_ = nullptr;
  }
  this->card_ = nullptr;
  this->mounted_ = false;
  if (this->card_detected_sensor_ != nullptr)
    this->card_detected_sensor_->publish_state(false);
  if (this->card_present_sensor_ != nullptr)
    this->card_present_sensor_->publish_state(false);
}

void Jc8012p4Storage::publish_stats_() {
  if (static_cast<esp_err_t>(this->stats_error_.load()) != ESP_OK) {
    ESP_LOGW(TAG, "SD volume statistics failed: %s", esp_err_to_name(static_cast<esp_err_t>(this->stats_error_.load())));
    return;
  }
  const uint64_t total = this->stats_total_.load();
  const uint64_t free = this->stats_free_.load();
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
