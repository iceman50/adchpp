from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication

    from adchpp_ui.window import ConfigWindow
except ImportError:
    QApplication = None


@unittest.skipIf(QApplication is None, "PyQt6 is not installed")
class ConfigWindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.application = QApplication.instance() or QApplication([])

    def test_initial_setup_save(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "config"
            window = ConfigWindow(directory)
            window.hub_name.setText("Portable test hub")
            window.user_nick.setText("owner")
            window.user_password.setText("secret")
            window.user_level.setText("10")
            window._add_user()
            window.custom_plugin_name.setText("Script")
            window._add_custom_plugin()
            window.plugin_settings_editor.setPlainText(
                '<ScriptPlugin><Engine language="lua" scriptPath="Scripts/"/></ScriptPlugin>'
            )
            window.save_all()
            self.application.processEvents()

            self.assertTrue((directory / "adchpp.xml").is_file())
            self.assertTrue((directory / "users.txt").is_file())
            self.assertTrue((directory / "Script.xml").is_file())
            users = json.loads((directory / "users.txt").read_text(encoding="utf-8"))
            self.assertEqual(users[0]["nick"], "owner")
            self.assertEqual(users[0]["level"], 10)
            self.assertEqual(
                window.plugins_table.item(0, 0).checkState(), Qt.CheckState.Checked
            )
            window.close()


if __name__ == "__main__":
    unittest.main()
