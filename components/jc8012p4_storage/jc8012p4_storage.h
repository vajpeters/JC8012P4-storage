#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/core/component.h"

#include <cstdio>
#include <string>

#include "sdmmc_cmd.h"

namespace esphome::jc8012p4_storage {

class Jc8012p4Storage : public Component, public tessera::Plugin {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_card_present_sensor(binary_sensor::BinarySensor *sensor) { this->card_present_sensor_ = sensor; }
  void set_total_space_sensor(sensor::Sensor *sensor) { this->total_space_sensor_ = sensor; }
  void set_free_space_sensor(sensor::Sensor *sensor) { this->free_space_sensor_ = sensor; }
  void set_used_percent_sensor(sensor::Sensor *sensor) { this->used_percent_sensor_ = sensor; }

  bool is_mounted() const { return this->mounted_; }
  FILE *open(const std::string &path, const char *mode = "rb");

 protected:
  bool mount_();
  void unmount_();
  void publish_stats_();
  bool make_path_(const std::string &path, std::string &absolute) const;

  binary_sensor::BinarySensor *card_present_sensor_{nullptr};
  sensor::Sensor *total_space_sensor_{nullptr};
  sensor::Sensor *free_space_sensor_{nullptr};
  sensor::Sensor *used_percent_sensor_{nullptr};
  sdmmc_card_t *card_{nullptr};
  bool mounted_{false};
  uint32_t last_check_{0};
};

}  // namespace esphome::jc8012p4_storage
