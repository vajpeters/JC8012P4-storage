"""Regression test for the ESP-IDF components required by SD storage."""

import asyncio
import importlib.util
import sys
import types
import unittest
from pathlib import Path

import esphome.codegen as cg
from esphome.components import esp32


COMPONENT = (
    Path(__file__).parents[1]
    / "components"
    / "jc8012p4_storage"
    / "__init__.py"
)


class IdfDependencyTest(unittest.TestCase):
    def test_storage_enables_fatfs_and_sdmmc(self):
        smart_display = types.ModuleType("esphome.components.smart_display")

        async def register_plugin(*_args):
            return None

        smart_display.register_plugin = register_plugin
        sys.modules["esphome.components.smart_display"] = smart_display

        spec = importlib.util.spec_from_file_location(
            "jc8012p4_storage_under_test", COMPONENT
        )
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)

        included = []
        fatfs_requested = []
        original_include = esp32.include_builtin_idf_component
        original_require_fatfs = esp32.require_fatfs
        original_new_pvariable = cg.new_Pvariable
        original_register_component = cg.register_component

        async def register_component(*_args):
            return None

        try:
            esp32.include_builtin_idf_component = included.append
            esp32.require_fatfs = lambda: fatfs_requested.append(True)
            cg.new_Pvariable = lambda *_args: object()
            cg.register_component = register_component

            asyncio.run(module.to_code({module.CONF_ID: object()}))
        finally:
            esp32.include_builtin_idf_component = original_include
            esp32.require_fatfs = original_require_fatfs
            cg.new_Pvariable = original_new_pvariable
            cg.register_component = original_register_component
            sys.modules.pop("esphome.components.smart_display", None)

        self.assertIn("fatfs", included)
        self.assertIn("esp_driver_sdmmc", included)
        self.assertIn("sdmmc", included)
        self.assertTrue(fatfs_requested)


if __name__ == "__main__":
    unittest.main()
