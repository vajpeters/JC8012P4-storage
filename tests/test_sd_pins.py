from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components" / "jc8012p4_storage" / "jc8012p4_storage.cpp"
HEADER = ROOT / "components" / "jc8012p4_storage" / "jc8012p4_storage.h"
CONFIG = ROOT / "components" / "jc8012p4_storage" / "__init__.py"


def test_tf_slot_uses_sd_spi_pins() -> None:
    source = SOURCE.read_text(encoding="utf-8")

    assert "bus_config.mosi_io_num = GPIO_NUM_44;" in source
    assert "bus_config.miso_io_num = GPIO_NUM_39;" in source
    assert "bus_config.sclk_io_num = GPIO_NUM_43;" in source
    assert "slot_config.gpio_cs = GPIO_NUM_42;" in source


def test_tf_slot_does_not_claim_the_wifi_coprocessor_bus() -> None:
    source = SOURCE.read_text(encoding="utf-8")

    for gpio in range(14, 20):
        assert f"GPIO_NUM_{gpio}" not in source


def test_sd_mount_waits_until_tessera_is_ready() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    setup = source[source.index("void Jc8012p4Storage::setup()") : source.index("void Jc8012p4Storage::on_ready()")]
    ready = source[source.index("void Jc8012p4Storage::on_ready()") : source.index("void Jc8012p4Storage::loop()")]

    assert "public tessera::Plugin" in header
    assert "setup_priority::LATE" in header
    assert "start_mount_()" not in setup
    assert "this->ready_ = true;" in ready
    assert "if (!this->ready_ || !this->probe_enabled_)" in source


def test_sd_probe_requires_explicit_enable() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")

    assert "void set_probe_enabled(bool enabled);" in header
    assert "bool probe_enabled_{false};" in header
    assert "SD card probe enabled by Home Assistant" in source


def test_card_detection_is_separate_from_mount_state() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    config = CONFIG.read_text(encoding="utf-8")

    assert "card_detected_sensor_" in header
    assert "CONF_CARD_DETECTED" in config
    assert "err != ESP_ERR_TIMEOUT" in source
    assert "card_detected_sensor_->publish_state(this->card_detected_.load())" in source


def test_tf_slot_enables_its_documented_ldo_supply() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")

    assert "SD_LDO_CHANNEL = 4" in source
    assert "SD_LDO_VOLTAGE_MV = 3300" in source
    assert "esp_ldo_acquire_channel" in source
    assert "esp_ldo_release_channel" in source
    assert "esp_ldo_channel_handle_t ldo_channel_" in header
