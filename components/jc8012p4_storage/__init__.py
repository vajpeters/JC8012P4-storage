"""SD-card support for the JC8012P4 Tessera panel."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, esp32, sensor, smart_display
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_DATA_SIZE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_DATABASE,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

DEPENDENCIES = ["smart_display"]
AUTO_LOAD = ["binary_sensor", "sensor"]

CONF_CARD_PRESENT = "card_present"
CONF_CARD_DETECTED = "card_detected"
CONF_FREE_SPACE = "free_space"
CONF_TOTAL_SPACE = "total_space"
CONF_USED_PERCENT = "used_percent"

jc8012p4_storage_ns = cg.esphome_ns.namespace("jc8012p4_storage")
Jc8012p4Storage = jc8012p4_storage_ns.class_(
    "Jc8012p4Storage", cg.Component
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Jc8012p4Storage),
            cv.Optional(CONF_CARD_PRESENT): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:sd",
            ),
            cv.Optional(CONF_CARD_DETECTED): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:sd",
            ),
            cv.Optional(CONF_TOTAL_SPACE): sensor.sensor_schema(
                unit_of_measurement="GB",
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_DATA_SIZE,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon=ICON_DATABASE,
            ),
            cv.Optional(CONF_FREE_SPACE): sensor.sensor_schema(
                unit_of_measurement="GB",
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_DATA_SIZE,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon=ICON_DATABASE,
            ),
            cv.Optional(CONF_USED_PERCENT): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:chart-donut",
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    esp32.include_builtin_idf_component("fatfs")
    esp32.include_builtin_idf_component("esp_driver_sdmmc")
    esp32.include_builtin_idf_component("esp_driver_spi")
    esp32.include_builtin_idf_component("sdmmc")
    esp32.require_fatfs()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)

    if card_present := config.get(CONF_CARD_PRESENT):
        sens = await binary_sensor.new_binary_sensor(card_present)
        cg.add(var.set_card_present_sensor(sens))
    if card_detected := config.get(CONF_CARD_DETECTED):
        sens = await binary_sensor.new_binary_sensor(card_detected)
        cg.add(var.set_card_detected_sensor(sens))
    if total_space := config.get(CONF_TOTAL_SPACE):
        sens = await sensor.new_sensor(total_space)
        cg.add(var.set_total_space_sensor(sens))
    if free_space := config.get(CONF_FREE_SPACE):
        sens = await sensor.new_sensor(free_space)
        cg.add(var.set_free_space_sensor(sens))
    if used_percent := config.get(CONF_USED_PERCENT):
        sens = await sensor.new_sensor(used_percent)
        cg.add(var.set_used_percent_sensor(sens))

