#include "jc8012p4_storage.h"

#include "esphome/core/log.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome::jc8012p4_storage {

static const char *const TAG = "jc8012p4_storage";
static constexpr const char *MOUNT_POINT = "/sdcard";
static constexpr uint32_t CHECK_INTERVAL_MS = 30000;
static constexpr uint32_t SPI_CLOCK_KHZ = 10000;  // Conservative starting speed for the board.
static constexpr int SD_LDO_CHANNEL = 4;

void Jc8012p4Storage::setup() { this->last_check_ = millis(); }

void Jc8012p4Storage::on_ready() {
  // The ESP32-P4 SD drivers must be initialized from the task on core 0.
  // Tessera plugin callbacks run on core 1, so do not mount synchronously.
  this->ready_ = true;
  this->start_mount_();
}

void Jc8012p4Storage::loop() {
  if (!this->ready_)
    return;

  // The worker has finished writing card_, error and other mount state.
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
  ESP_LOGCONFIG(TAG, "  Interface: SPI, %u kHz", static_cast<unsigned>(SPI_CLOCK_KHZ));
  ESP_LOGCONFIG(TAG, "  Pins: CLK 43, MOSI/CMD 44, MISO/D0 39, CS/D3 42");
  ESP_LOGCONFIG(TAG, "  SD IO power: on-chip LDO channel %d", SD_LDO_CHANNEL);
  ESP_LOGCONFIG(TAG, "  Mounted: %s", YESNO(this->mounted_));
  LOG_BINARY_SENSOR("  ", "Card mounted", this->card_present_sensor_);
  LOG_BINARY_SENSOR("  ", "Card detected", this->card_detected_sensor_);
  LOG_SENSOR("  ", "Total space", this->total_space_sensor_);
  LOG_SENSOR("  ", "Free space", this->free_space_sensor_);
  LOG_SENSOR("  ", "Used", this->used_percent_sensor_);
}

void Jc8012p4Storage::on_shutdown() {
  // Do not destroy drivers while the mounting task is still using them.
  if (!this->mounting_.load())
    this->unmount_();
}

void Jc8012p4Storage::start_mount_() {
  bool expected = false;
  if (!this->mounting_.compare_exchange_strong(expected, true))
    return;

  this->mount_finished_.store(false);
  ESP_LOGI(TAG, "Starting SD card probe");
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
