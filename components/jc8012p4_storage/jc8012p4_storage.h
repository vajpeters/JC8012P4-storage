#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/core/component.h"

#include <atomic>
#include <cstdio>
#include <string>

#include "esp_err.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

namespace esphome::jc8012p4_storage {

class Jc8012p4Storage : public Component, public tessera::Plugin {
 public:
  void setup() override;
  void loop() override;
  void on_ready() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_card_present_sensor(binary_sensor::BinarySensor *sensor) { this->card_present_sensor_ = sensor; }
  void set_card_detected_sensor(binary_sensor::BinarySensor *sensor) { this->card_detected_sensor_ = sensor; }
  void set_total_space_sensor(sensor::Sensor *sensor) { this->total_space_sensor_ = sensor; }
  void set_free_space_sensor(sensor::Sensor *sensor) { this->free_space_sensor_ = sensor; }
  void set_used_percent_sensor(sensor::Sensor *sensor) { this->used_percent_sensor_ = sensor; }

  bool is_mounted() const { return this->mounted_; }
  FILE *open(const std::string &path, const char *mode = "rb");

 protected:
  void start_mount_();
  static void mount_task_(void *parameter);
  esp_err_t mount_card_();
  void finish_mount_();
  void unmount_();
  void release_bus_();
  void publish_stats_();
  void clear_stats_();
  bool make_path_(const std::string &path, std::string &absolute) const;

  binary_sensor::BinarySensor *card_present_sensor_{nullptr};
  binary_sensor::BinarySensor *card_detected_sensor_{nullptr};
  sensor::Sensor *total_space_sensor_{nullptr};
  sensor::Sensor *free_space_sensor_{nullptr};
  sensor::Sensor *used_percent_sensor_{nullptr};

  sdmmc_card_t *card_{nullptr};
  sd_pwr_ctrl_handle_t pwr_ctrl_handle_{nullptr};
  int spi_host_slot_{-1};
  bool mounted_{false};
  bool ready_{false};

  // Only these atomics cross the core-0 mounting task / ESPHome loop boundary.
  std::atomic<bool> mounting_{false};
  std::atomic<bool> mount_finished_{false};
  std::atomic<int> mount_error_{ESP_FAIL};
  std::atomic<bool> card_detected_{false};
  uint32_t last_check_{0};
};

}  // namespace esphome::jc8012p4_storage
