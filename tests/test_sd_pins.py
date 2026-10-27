from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components" / "jc8012p4_storage" / "jc8012p4_storage.cpp"


def test_tf_slot_uses_the_dedicated_sdmmc_bus() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    expected = {
        "clk": 43,
        "cmd": 44,
        "d0": 39,
        "d1": 40,
        "d2": 41,
        "d3": 42,
    }

    for signal, gpio in expected.items():
        assert f"slot_config.{signal} = GPIO_NUM_{gpio};" in source


def test_tf_slot_does_not_claim_the_wifi_coprocessor_bus() -> None:
    source = SOURCE.read_text(encoding="utf-8")

    for gpio in range(14, 20):
        assert f"GPIO_NUM_{gpio}" not in source


def test_sd_mount_waits_until_tessera_is_ready() -> None:
    header = (ROOT / "components/jc8012p4_storage/jc8012p4_storage.h").read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    setup = source[source.index("void Jc8012p4Storage::setup()") : source.index("void Jc8012p4Storage::on_ready()")]
    ready = source[source.index("void Jc8012p4Storage::on_ready()") : source.index("void Jc8012p4Storage::loop()")]

    assert "public tessera::Plugin" in header
    assert "setup_priority::LATE" in header
    assert "mount_()" not in setup
    assert "this->mount_();" in ready
    assert "if (!this->ready_)" in source
