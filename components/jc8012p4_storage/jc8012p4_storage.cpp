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
  this->card_detected_.store(false);

  esp_vfs_fat_sdmmc_mount_config_t mount_config{};
  mount_config.format_if_mount_failed = false;  // Never erase the user's card.
  mount_config.max_files = 5;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = SPI_CLOCK_KHZ;

  // CRITICAL FOR ESP32-P4: GPIO39-44 are in the SD IO power domain.
  // The Guition board uses on-chip LDO channel 4. Without it the card
  // cannot answer commands although it is physically present.
  sd_pwr_ctrl_ldo_config_t ldo_config{};
  ldo_config.ldo_chan_id = SD_LDO_CHANNEL;
  esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &this->pwr_ctrl_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SD LDO channel %d failed: %s (0x%x)", SD_LDO_CHANNEL,
             esp_err_to_name(err), static_cast<unsigned>(err));
    this->pwr_ctrl_handle_ = nullptr;
    return err;
  }
  host.pwr_ctrl_handle = this->pwr_ctrl_handle_;

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
    ESP_LOGE(TAG, "SPI bus init failed: %s (0x%x)", esp_err_to_name(err), static_cast<unsigned>(err));
    this->spi_host_slot_ = -1;  // SPI bus not owned by us.
    this->release_bus_();
    return err;
  }

  // The JC8012P4A1 may have no external SD pull-up resistors.
  // Configure them AFTER SPI initializes the pins, BEFORE card probing.
  // Pull CMD/DAT0-DAT3 high and the clock line as the board example expects.
  // GPIO40/41 are unused in SPI mode but still connected to the SD socket.
  for (gpio_num_t pin : {GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41,
                         GPIO_NUM_42, GPIO_NUM_43, GPIO_NUM_44}) {
    err = gpio_pullup_en(pin);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Pull-up on GPIO%d failed: %s (0x%x)", static_cast<int>(pin),
               esp_err_to_name(err), static_cast<unsigned>(err));
      this->release_bus_();
      return err;
    }
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = GPIO_NUM_42;
  slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

  ESP_LOGI(TAG, "Probing SD at %s via SPI (LDO4 + pull-ups)", MOUNT_POINT);
  err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &this->card_);

  // No mechanical card-detect switch exists on the TF socket.
  // ESP_FAIL means a card generally responded but the filesystem mount failed;
  // a timeout or a driver error does not establish physical card presence.
  this->card_detected_.store(err == ESP_OK || err == ESP_FAIL);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SD card mount/init failed: %s (0x%x)", esp_err_to_name(err),
             static_cast<unsigned>(err));
    this->card_ = nullptr;
    this->release_bus_();
    return err;
  }
  return ESP_OK;
}

void Jc8012p4Storage::finish_mount_() {
  const esp_err_t err = static_cast<esp_err_t>(this->mount_error_.load());
  if (this->card_detected_sensor_ != nullptr)
    this->card_detected_sensor_->publish_state(this->card_detected_.load());
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SD card unavailable: %s (0x%x), retrying in %u s",
             esp_err_to_name(err), static_cast<unsigned>(err),
             static_cast<unsigned>(CHECK_INTERVAL_MS / 1000));
    this->mounted_ = false;
    this->card_ = nullptr;
    if (this->card_present_sensor_ != nullptr)
      this->card_present_sensor_->publish_state(false);
    this->clear_stats_();
    return;
  }

  this->mounted_ = true;
  if (this->card_present_sensor_ != nullptr)
    this->card_present_sensor_->publish_state(true);
  ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
  sdmmc_card_print_info(stdout, this->card_);
  this->publish_stats_();
}

void Jc8012p4Storage::release_bus_() {
  // Called only when no card is mounted, or after a successful unmount.
  if (this->spi_host_slot_ >= 0) {
    const esp_err_t err = spi_bus_free(static_cast<spi_host_device_t>(this->spi_host_slot_));
    if (err != ESP_OK)
      ESP_LOGW(TAG, "SPI bus cleanup failed: %s", esp_err_to_name(err));
    this->spi_host_slot_ = -1;
  }
  if (this->pwr_ctrl_handle_ != nullptr) {
    const esp_err_t err = sd_pwr_ctrl_del_on_chip_ldo(this->pwr_ctrl_handle_);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "SD LDO cleanup failed: %s", esp_err_to_name(err));
    this->pwr_ctrl_handle_ = nullptr;
  }
}

void Jc8012p4Storage::unmount_() {
  if (this->mounted_) {
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, this->card_);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "SD unmount failed: %s", esp_err_to_name(err));
  }
  this->card_ = nullptr;
  this->mounted_ = false;
  this->release_bus_();
  this->card_detected_.store(false);
  if (this->card_detected_sensor_ != nullptr)
    this->card_detected_sensor_->publish_state(false);
  if (this->card_present_sensor_ != nullptr)
    this->card_present_sensor_->publish_state(false);
  this->clear_stats_();
}

void Jc8012p4Storage::clear_stats_() {
  if (this->total_space_sensor_ != nullptr)
    this->total_space_sensor_->publish_state(NAN);
  if (this->free_space_sensor_ != nullptr)
    this->free_space_sensor_->publish_state(NAN);
  if (this->used_percent_sensor_ != nullptr)
    this->used_percent_sensor_->publish_state(NAN);
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
  if (path.empty() || path.find("..") != std::string::npos || path.find('\\') != std::string::npos)
    return false;

  if (path.rfind("sd://", 0) == 0)
    absolute = std::string(MOUNT_POINT) + "/" + path.substr(5);
  else if (path.rfind("file:///sdcard/", 0) == 0)
    absolute = path.substr(7);
  else if (path.rfind("/sdcard/", 0) == 0)
    absolute = path;
  else
    return false;

  // Prevent a request for just the mount directory instead of a file.
  return absolute.size() > std::strlen(MOUNT_POINT) + 1;
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
