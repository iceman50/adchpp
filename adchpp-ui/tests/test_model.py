from __future__ import annotations

import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from adchpp_ui.model import ConfigError, ConfigModel, run_model_self_test


class ConfigModelTests(unittest.TestCase):
    def test_internal_round_trip(self) -> None:
        run_model_self_test()

    def test_preserves_unknown_core_and_user_data(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "adchpp.xml").write_text(
                """<?xml version="1.0"?>
<ADCHubPlusPlus>
  <Settings><HubName type="string">Before</HubName><Future>keep</Future></Settings>
  <Servers><Server Port="2780"/></Servers>
  <Plugins><Plugin>Script</Plugin></Plugins>
  <Custom answer="42"/>
</ADCHubPlusPlus>
""",
                encoding="utf-8",
            )
            (directory / "users.txt").write_text(
                json.dumps(
                    [
                        {
                            "nick": "owner",
                            "password": "secret",
                            "level": 10,
                            "future": {"nested": [True, None, 3.5]},
                        }
                    ]
                ),
                encoding="utf-8",
            )

            model = ConfigModel()
            self.assertTrue(model.load(directory))
            model.settings.hub_name = "After"
            model.save()

            root = ET.parse(directory / "adchpp.xml").getroot()
            self.assertEqual(root.findtext("Settings/Future"), "keep")
            self.assertEqual(root.find("Custom").get("answer"), "42")
            users = json.loads((directory / "users.txt").read_text(encoding="utf-8"))
            self.assertEqual(users[0]["future"], {"nested": [True, None, 3.5]})

    def test_rejects_bad_values_without_writing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            model = ConfigModel()
            model.load(directory)
            model.settings.buffer_size = "65536"
            model.settings.max_buffer_size = "4096"
            with self.assertRaises(ConfigError):
                model.save()
            self.assertFalse((directory / "adchpp.xml").exists())


if __name__ == "__main__":
    unittest.main()
