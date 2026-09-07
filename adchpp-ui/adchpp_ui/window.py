"""PyQt6 widgets for the ADCH++ configuration manager."""

from __future__ import annotations

import copy
import os
import time
from pathlib import Path

from PyQt6.QtCore import QSignalBlocker, Qt
from PyQt6.QtGui import QIntValidator
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
    QTabWidget,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .model import ConfigError, ConfigModel, ListenerSettings, is_safe_plugin_name


def _line_edit(minimum: int | None = None, maximum: int | None = None) -> QLineEdit:
    editor = QLineEdit()
    if minimum is not None and maximum is not None:
        editor.setValidator(QIntValidator(minimum, maximum, editor))
    return editor


def _table(columns: list[str]) -> QTableWidget:
    table = QTableWidget(0, len(columns))
    table.setHorizontalHeaderLabels(columns)
    table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
    table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
    table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
    table.setAlternatingRowColors(True)
    table.verticalHeader().setVisible(False)
    table.horizontalHeader().setStretchLastSection(True)
    return table


def _item(text: object) -> QTableWidgetItem:
    return QTableWidgetItem(str(text))


class ConfigWindow(QMainWindow):
    def __init__(self, initial_directory: Path) -> None:
        super().__init__()
        self.model = ConfigModel()
        self._plugin_names: list[str] = []
        self._selected_plugin = -1
        self.setWindowTitle("ADCH++ Configuration Manager")
        self.resize(1280, 820)
        self.setMinimumSize(940, 640)
        self._build()
        self.load_directory(initial_directory)

    def _build(self) -> None:
        central = QWidget()
        root = QVBoxLayout(central)

        path_row = QHBoxLayout()
        path_row.addWidget(QLabel("Configuration folder:"))
        self.config_path = QLineEdit()
        path_row.addWidget(self.config_path, 1)
        open_button = QPushButton("Open")
        browse_button = QPushButton("Browse…")
        path_row.addWidget(open_button)
        path_row.addWidget(browse_button)
        path_row.addWidget(QLabel("Stop the hub before saving changes."))
        root.addLayout(path_row)

        self.tabs = QTabWidget()
        self.tabs.addTab(self._build_core_page(), "Core settings")
        self.tabs.addTab(self._build_listeners_page(), "Listeners and TLS")
        self.tabs.addTab(self._build_users_page(), "Registered users")
        self.tabs.addTab(self._build_plugins_page(), "Plugins")
        root.addWidget(self.tabs, 1)

        buttons = QHBoxLayout()
        self.status_label = QLabel("Ready")
        buttons.addWidget(self.status_label, 1)
        reload_button = QPushButton("Reload")
        save_button = QPushButton("Save all")
        save_button.setDefault(True)
        exit_button = QPushButton("Exit")
        buttons.addWidget(reload_button)
        buttons.addWidget(save_button)
        buttons.addWidget(exit_button)
        root.addLayout(buttons)
        self.setCentralWidget(central)

        open_button.clicked.connect(lambda: self.load_directory(self.config_path.text()))
        browse_button.clicked.connect(self._browse_config_directory)
        reload_button.clicked.connect(self._reload_directory)
        save_button.clicked.connect(self.save_all)
        exit_button.clicked.connect(self.close)

    def _build_core_page(self) -> QWidget:
        page = QWidget()
        layout = QGridLayout(page)
        self.hub_name = QLineEdit()
        self.description = QLineEdit()
        self.log_enabled = QCheckBox("Write hub and plugin logs")
        self.log_file = QLineEdit()
        self.max_command_size = _line_edit(0, 2**31 - 1)
        self.buffer_size = _line_edit(256, 2**31 - 1)
        self.max_buffer_size = _line_edit(256, 2**31 - 1)
        self.overflow_timeout = _line_edit(0, 2**31 - 1)
        self.overflow_limit = _line_edit(0, 100)
        self.disconnect_timeout = _line_edit(0, 2**31 - 1)
        self.log_timeout = _line_edit(0, 2**31 - 1)

        rows = (
            ("Hub name:", self.hub_name, "Description:", self.description),
            ("Logging:", self.log_enabled, "Log file pattern:", self.log_file),
            ("Max command bytes:", self.max_command_size, "Initial buffer bytes:", self.buffer_size),
            ("Max buffer bytes:", self.max_buffer_size, "Overflow timeout (ms):", self.overflow_timeout),
            ("Overflow user limit (%):", self.overflow_limit, "Disconnect timeout (ms):", self.disconnect_timeout),
            ("Login timeout (ms):", self.log_timeout, "", None),
        )
        for row, (left_label, left, right_label, right) in enumerate(rows):
            layout.addWidget(QLabel(left_label), row, 0)
            layout.addWidget(left, row, 1)
            if right is not None:
                layout.addWidget(QLabel(right_label), row, 2)
                layout.addWidget(right, row, 3)
        layout.addWidget(
            QLabel("Connection limits are validated before save. Buffer sizes must be at least 256 bytes."),
            len(rows),
            0,
            1,
            4,
        )
        layout.setRowStretch(len(rows) + 1, 1)
        layout.setColumnStretch(1, 1)
        layout.setColumnStretch(3, 1)
        return page

    def _build_listeners_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        splitter = QSplitter()
        self.listeners_table = _table(["Port", "TLS", "Bind addresses", "Public addresses"])
        self.listeners_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.ResizeToContents
        )
        self.listeners_table.horizontalHeader().setStretchLastSection(True)
        splitter.addWidget(self.listeners_table)

        form_box = QGroupBox("Selected listener")
        form = QFormLayout(form_box)
        self.listener_port = _line_edit(1, 65535)
        self.listener_tls = QCheckBox("Enable TLS")
        self.listener_bind = QLineEdit()
        self.listener_bind4 = QLineEdit()
        self.listener_bind6 = QLineEdit()
        self.listener_hub4 = QLineEdit()
        self.listener_hub6 = QLineEdit()
        self.listener_certificate = QLineEdit()
        self.listener_private_key = QLineEdit()
        self.listener_trusted_path = QLineEdit()
        self.listener_dh_params = QLineEdit()
        self.listener_min_version = QComboBox()
        self.listener_min_version.addItems(
            ["TLS 1.0 (0)", "TLS 1.1 (1)", "TLS 1.2 (2)", "TLS 1.3 (3)"]
        )
        self.listener_security_level = _line_edit(0, 5)
        self.listener_cipher_suites = QLineEdit()
        for label, widget in (
            ("Port:", self.listener_port),
            ("Transport:", self.listener_tls),
            ("Legacy bind:", self.listener_bind),
            ("Bind IPv4:", self.listener_bind4),
            ("Bind IPv6:", self.listener_bind6),
            ("Public IPv4:", self.listener_hub4),
            ("Public IPv6:", self.listener_hub6),
            ("Certificate:", self.listener_certificate),
            ("Private key:", self.listener_private_key),
            ("Trusted path:", self.listener_trusted_path),
            ("DH parameters:", self.listener_dh_params),
            ("Minimum version:", self.listener_min_version),
            ("Security level (0–5):", self.listener_security_level),
            ("TLS 1.3 cipher suites:", self.listener_cipher_suites),
        ):
            form.addRow(label, widget)
        splitter.addWidget(form_box)
        splitter.setSizes([560, 560])
        layout.addWidget(splitter, 1)

        row = QHBoxLayout()
        add_button = QPushButton("Add listener")
        update_button = QPushButton("Update selected")
        remove_button = QPushButton("Remove selected")
        row.addWidget(add_button)
        row.addWidget(update_button)
        row.addWidget(remove_button)
        row.addStretch(1)
        row.addWidget(QLabel("Public addresses are used by HBRI and may be left blank."))
        layout.addLayout(row)

        self.listeners_table.itemSelectionChanged.connect(self._select_listener)
        add_button.clicked.connect(self._add_listener)
        update_button.clicked.connect(self._update_listener)
        remove_button.clicked.connect(self._remove_listener)
        return page

    def _build_users_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        splitter = QSplitter()
        self.users_table = _table(["Nickname", "Level", "CID (optional)"])
        self.users_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.ResizeToContents
        )
        self.users_table.horizontalHeader().setStretchLastSection(True)
        splitter.addWidget(self.users_table)

        form_box = QGroupBox("Registration")
        form = QFormLayout(form_box)
        self.user_nick = QLineEdit()
        self.user_cid = QLineEdit()
        self.user_password = QLineEdit()
        self.user_password.setEchoMode(QLineEdit.EchoMode.Password)
        self.user_level = _line_edit(1, 2**31 - 1)
        self.user_level.setText("1")
        form.addRow("Nickname:", self.user_nick)
        form.addRow("CID:", self.user_cid)
        form.addRow("Password:", self.user_password)
        form.addRow("Access level:", self.user_level)
        form.addRow(
            QLabel(
                "Passwords are stored in clear text because that is the format used by the bundled access.lua script."
            )
        )
        splitter.addWidget(form_box)
        splitter.setSizes([620, 500])
        layout.addWidget(splitter, 1)

        row = QHBoxLayout()
        add_button = QPushButton("Add user")
        update_button = QPushButton("Update selected")
        remove_button = QPushButton("Remove selected")
        clear_button = QPushButton("Clear form")
        for button in (add_button, update_button, remove_button, clear_button):
            row.addWidget(button)
        row.addStretch(1)
        row.addWidget(QLabel("A nickname-only registration receives its CID after login."))
        layout.addLayout(row)

        self.users_table.itemSelectionChanged.connect(self._select_user)
        add_button.clicked.connect(self._add_user)
        update_button.clicked.connect(self._update_user)
        remove_button.clicked.connect(self._remove_user)
        clear_button.clicked.connect(self._clear_user_form)
        return page

    def _build_plugins_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        path_row = QHBoxLayout()
        path_row.addWidget(QLabel("Plugin library path:"))
        self.plugin_path = QLineEdit()
        path_row.addWidget(self.plugin_path, 1)
        browse_button = QPushButton("Browse…")
        path_row.addWidget(browse_button)
        layout.addLayout(path_row)

        splitter = QSplitter()
        plugin_side = QWidget()
        plugin_layout = QVBoxLayout(plugin_side)
        self.plugins_table = _table(["Checked plugins load in this order"])
        plugin_layout.addWidget(self.plugins_table, 1)
        order_row = QHBoxLayout()
        up_button = QPushButton("Move up")
        down_button = QPushButton("Move down")
        order_row.addWidget(up_button)
        order_row.addWidget(down_button)
        plugin_layout.addLayout(order_row)
        custom_row = QHBoxLayout()
        self.custom_plugin_name = QLineEdit()
        self.custom_plugin_name.setPlaceholderText("Custom plugin name")
        add_button = QPushButton("Add plugin")
        custom_row.addWidget(self.custom_plugin_name, 1)
        custom_row.addWidget(add_button)
        plugin_layout.addLayout(custom_row)
        splitter.addWidget(plugin_side)

        settings_side = QWidget()
        settings_layout = QVBoxLayout(settings_side)
        self.plugin_settings_label = QLabel("Select a plugin to edit its XML settings.")
        self.plugin_settings_editor = QPlainTextEdit()
        self.plugin_settings_editor.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        settings_layout.addWidget(self.plugin_settings_label)
        settings_layout.addWidget(self.plugin_settings_editor, 1)
        splitter.addWidget(settings_side)
        splitter.setSizes([440, 680])
        layout.addWidget(splitter, 1)

        self.plugins_table.currentCellChanged.connect(self._plugin_selection_changed)
        browse_button.clicked.connect(self._browse_plugin_directory)
        add_button.clicked.connect(self._add_custom_plugin)
        up_button.clicked.connect(lambda: self._move_plugin(-1))
        down_button.clicked.connect(lambda: self._move_plugin(1))
        return page

    def _show_error(self, message: str) -> None:
        QMessageBox.critical(self, "ADCH++ configuration error", message)
        self._set_status("Operation failed. Review the error before retrying.")

    def _set_status(self, message: str) -> None:
        self.status_label.setText(message)

    def load_directory(self, directory: str | os.PathLike[str]) -> None:
        try:
            existing = self.model.load(directory)
            self.config_path.setText(str(self.model.config_directory))
            self._fill_core()
            self._refresh_listeners()
            self._refresh_users()
            self._refresh_plugins()
            self._set_status(
                "Configuration loaded. Stop the hub before saving changes."
                if existing
                else "New setup: defaults are ready. Save all to create the files."
            )
        except (ConfigError, OSError, UnicodeError) as exc:
            self._show_error(str(exc))

    def _browse_config_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(
            self, "Select the ADCH++ configuration folder", self.config_path.text()
        )
        if directory:
            self.load_directory(directory)

    def _reload_directory(self) -> None:
        answer = QMessageBox.question(
            self,
            "ADCH++ Configuration Manager",
            "Discard unsaved edits and reload this configuration folder?",
        )
        if answer == QMessageBox.StandardButton.Yes:
            self.load_directory(self.config_path.text())

    def _fill_core(self) -> None:
        settings = self.model.settings
        self.hub_name.setText(settings.hub_name)
        self.description.setText(settings.description)
        self.log_enabled.setChecked(settings.log_enabled)
        self.log_file.setText(settings.log_file)
        self.max_command_size.setText(settings.max_command_size)
        self.buffer_size.setText(settings.buffer_size)
        self.max_buffer_size.setText(settings.max_buffer_size)
        self.overflow_timeout.setText(settings.overflow_timeout)
        self.overflow_limit.setText(settings.overflow_limit)
        self.disconnect_timeout.setText(settings.disconnect_timeout)
        self.log_timeout.setText(settings.log_timeout)

    def _gather_core(self) -> None:
        settings = self.model.settings
        settings.hub_name = self.hub_name.text()
        settings.description = self.description.text()
        settings.log_enabled = self.log_enabled.isChecked()
        settings.log_file = self.log_file.text()
        settings.max_command_size = self.max_command_size.text()
        settings.buffer_size = self.buffer_size.text()
        settings.max_buffer_size = self.max_buffer_size.text()
        settings.overflow_timeout = self.overflow_timeout.text()
        settings.overflow_limit = self.overflow_limit.text()
        settings.disconnect_timeout = self.disconnect_timeout.text()
        settings.log_timeout = self.log_timeout.text()

    def _refresh_listeners(self, selected: int = 0) -> None:
        with QSignalBlocker(self.listeners_table):
            self.listeners_table.setRowCount(len(self.model.listeners))
            for row, listener in enumerate(self.model.listeners):
                binds = " / ".join(
                    value
                    for value in (
                        listener.bind_address,
                        listener.bind_address4,
                        listener.bind_address6,
                    )
                    if value
                )
                public = " / ".join(
                    value for value in (listener.hub_address4, listener.hub_address6) if value
                )
                for column, text in enumerate(
                    (listener.port, "Yes" if listener.tls else "No", binds, public)
                ):
                    self.listeners_table.setItem(row, column, _item(text))
            if self.model.listeners:
                selected = min(max(selected, 0), len(self.model.listeners) - 1)
                self.listeners_table.selectRow(selected)
        if self.model.listeners:
            self._fill_listener_form(self.model.listeners[selected])

    def _fill_listener_form(self, listener: ListenerSettings) -> None:
        self.listener_port.setText(listener.port)
        self.listener_bind.setText(listener.bind_address)
        self.listener_bind4.setText(listener.bind_address4)
        self.listener_bind6.setText(listener.bind_address6)
        self.listener_hub4.setText(listener.hub_address4)
        self.listener_hub6.setText(listener.hub_address6)
        self.listener_tls.setChecked(listener.tls)
        self.listener_certificate.setText(listener.certificate)
        self.listener_private_key.setText(listener.private_key)
        self.listener_trusted_path.setText(listener.trusted_path)
        self.listener_dh_params.setText(listener.dh_params)
        try:
            version = int(listener.min_version)
        except ValueError:
            version = 2
        self.listener_min_version.setCurrentIndex(min(max(version, 0), 3))
        self.listener_security_level.setText(listener.security_level)
        self.listener_cipher_suites.setText(listener.cipher_suites13)

    def _read_listener_form(self) -> ListenerSettings:
        return ListenerSettings(
            port=self.listener_port.text(),
            bind_address=self.listener_bind.text(),
            bind_address4=self.listener_bind4.text(),
            bind_address6=self.listener_bind6.text(),
            hub_address4=self.listener_hub4.text(),
            hub_address6=self.listener_hub6.text(),
            tls=self.listener_tls.isChecked(),
            certificate=self.listener_certificate.text(),
            private_key=self.listener_private_key.text(),
            trusted_path=self.listener_trusted_path.text(),
            dh_params=self.listener_dh_params.text(),
            min_version=str(self.listener_min_version.currentIndex()),
            security_level=self.listener_security_level.text(),
            cipher_suites13=self.listener_cipher_suites.text(),
        )

    def _select_listener(self) -> None:
        row = self.listeners_table.currentRow()
        if 0 <= row < len(self.model.listeners):
            self._fill_listener_form(self.model.listeners[row])

    def _add_listener(self) -> None:
        self.model.listeners.append(self._read_listener_form())
        self._refresh_listeners(len(self.model.listeners) - 1)
        self._set_status("Listener added in memory. Save all to persist it.")

    def _update_listener(self, checked: bool = False, quiet: bool = False) -> None:
        del checked
        row = self.listeners_table.currentRow()
        if not 0 <= row < len(self.model.listeners):
            if not quiet:
                self._show_error("Select a listener to update")
            return
        self.model.listeners[row] = self._read_listener_form()
        self._refresh_listeners(row)
        if not quiet:
            self._set_status("Listener updated in memory. Save all to persist it.")

    def _remove_listener(self) -> None:
        row = self.listeners_table.currentRow()
        if not 0 <= row < len(self.model.listeners):
            return
        if len(self.model.listeners) == 1:
            self._show_error("At least one listener is required")
            return
        del self.model.listeners[row]
        self._refresh_listeners(min(row, len(self.model.listeners) - 1))
        self._set_status("Listener removed in memory. Save all to persist it.")

    def _refresh_users(self, selected: int | None = None) -> None:
        with QSignalBlocker(self.users_table):
            self.users_table.setRowCount(len(self.model.users))
            for row, user in enumerate(self.model.users):
                for column, text in enumerate(
                    (user.get("nick", ""), user.get("level", ""), user.get("cid", ""))
                ):
                    self.users_table.setItem(row, column, _item(text))
            if selected is not None and self.model.users:
                selected = min(max(selected, 0), len(self.model.users) - 1)
                self.users_table.selectRow(selected)
        if selected is None:
            self._clear_user_form()
        elif self.model.users:
            self._fill_user_form(self.model.users[selected])

    def _clear_user_form(self) -> None:
        with QSignalBlocker(self.users_table):
            self.users_table.clearSelection()
            self.users_table.setCurrentCell(-1, -1)
        self.user_nick.clear()
        self.user_cid.clear()
        self.user_password.clear()
        self.user_level.setText("1")

    def _select_user(self) -> None:
        row = self.users_table.currentRow()
        if 0 <= row < len(self.model.users):
            self._fill_user_form(self.model.users[row])

    def _fill_user_form(self, user: dict[str, object]) -> None:
        self.user_nick.setText(str(user.get("nick", "")))
        self.user_cid.setText(str(user.get("cid", "")))
        self.user_password.setText(str(user.get("password", "")))
        self.user_level.setText(str(user.get("level", "")))

    def _apply_user_form(self, user: dict[str, object], new_user: bool) -> None:
        nick = self.user_nick.text()
        password = self.user_password.text()
        level_text = self.user_level.text()
        if not nick or not password or not level_text:
            raise ConfigError("Nickname, password, and access level are required")
        if not level_text.isdecimal():
            raise ConfigError("Access level must be a whole number")
        user["nick"] = nick
        user["password"] = password
        user["level"] = int(level_text)
        cid = self.user_cid.text().upper()
        if cid:
            user["cid"] = cid
        else:
            user.pop("cid", None)
        if new_user:
            user["regby"] = nick
            user["regtime"] = int(time.time())

    def _add_user(self) -> None:
        try:
            user: dict[str, object] = {}
            self._apply_user_form(user, True)
            self.model.users.append(user)
            self._refresh_users(len(self.model.users) - 1)
            self._set_status("Registered user added in memory. Save all to update users.txt.")
        except ConfigError as exc:
            self._show_error(str(exc))

    def _update_user(self, checked: bool = False, quiet: bool = False) -> None:
        del checked
        row = self.users_table.currentRow()
        if not 0 <= row < len(self.model.users):
            if not quiet:
                self._show_error("Select a registered user to update")
            return
        try:
            user = copy.deepcopy(self.model.users[row])
            self._apply_user_form(user, False)
            self.model.users[row] = user
            self._refresh_users(row)
            if not quiet:
                self._set_status("Registered user updated in memory. Save all to update users.txt.")
        except ConfigError as exc:
            if quiet:
                raise
            self._show_error(str(exc))

    def _remove_user(self) -> None:
        row = self.users_table.currentRow()
        if not 0 <= row < len(self.model.users):
            return
        nick = self.model.users[row].get("nick", "this user")
        answer = QMessageBox.question(
            self, "ADCH++ Configuration Manager", f"Remove the registration for {nick}?"
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        del self.model.users[row]
        self._refresh_users()
        self._set_status("Registered user removed in memory. Save all to update users.txt.")

    def _commit_plugin_settings(self, row: int | None = None) -> None:
        if row is None:
            row = self._selected_plugin
        if not 0 <= row < len(self._plugin_names):
            return
        name = self._plugin_names[row]
        if not is_safe_plugin_name(name):
            return
        text = self.plugin_settings_editor.toPlainText()
        if text.strip() or name in self.model.plugin_settings:
            self.model.plugin_settings[name] = text

    def _plugin_selection_changed(
        self, current_row: int, current_column: int, previous_row: int, previous_column: int
    ) -> None:
        del current_column, previous_column
        self._commit_plugin_settings(previous_row)
        self._selected_plugin = current_row
        self._show_plugin_settings(current_row)

    def _show_plugin_settings(self, row: int) -> None:
        if not 0 <= row < len(self._plugin_names):
            self.plugin_settings_label.setText("Select a plugin to edit its XML settings.")
            self.plugin_settings_editor.clear()
            self.plugin_settings_editor.setReadOnly(False)
            return
        name = self._plugin_names[row]
        if not is_safe_plugin_name(name):
            self.plugin_settings_label.setText(
                "Settings editing is unavailable for a path-based plugin entry."
            )
            self.plugin_settings_editor.clear()
            self.plugin_settings_editor.setReadOnly(True)
            return
        self.plugin_settings_label.setText(f"{name}.xml settings (raw XML):")
        self.plugin_settings_editor.setReadOnly(False)
        self.plugin_settings_editor.setPlainText(self.model.plugin_settings.get(name, ""))

    def _refresh_plugins(self, selected: int = 0) -> None:
        names = self.model.configured_plugin_names()
        for name in self.model.discover_plugin_names():
            if name not in names:
                names.append(name)
        self._plugin_names = names
        with QSignalBlocker(self.plugins_table):
            self.plugins_table.setRowCount(len(names))
            for row, name in enumerate(names):
                item = _item(name)
                item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
                item.setCheckState(
                    Qt.CheckState.Checked
                    if name in self.model.plugins
                    else Qt.CheckState.Unchecked
                )
                self.plugins_table.setItem(row, 0, item)
            if names:
                selected = min(max(selected, 0), len(names) - 1)
                self.plugins_table.setCurrentCell(selected, 0)
        self.plugin_path.setText(self.model.plugin_path)
        self._selected_plugin = selected if names else -1
        self._show_plugin_settings(self._selected_plugin)

    def _gather_plugins(self) -> None:
        self._commit_plugin_settings()
        self.model.plugin_path = self.plugin_path.text()
        self.model.plugins = [
            self._plugin_names[row]
            for row in range(len(self._plugin_names))
            if self.plugins_table.item(row, 0).checkState() == Qt.CheckState.Checked
        ]

    def _browse_plugin_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(
            self, "Select the directory containing ADCH++ plugin libraries", self.plugin_path.text()
        )
        if not directory:
            return
        self._gather_plugins()
        self.model.plugin_path = directory.rstrip("/\\") + os.sep
        self._refresh_plugins()
        self._set_status("Plugin path updated in memory. Select plugins to load, then save all.")

    def _add_custom_plugin(self) -> None:
        name = self.custom_plugin_name.text()
        if not is_safe_plugin_name(name):
            self._show_error(
                "Use only letters, numbers, dots, dashes, and underscores in a custom plugin name"
            )
            return
        if name in self._plugin_names:
            self._show_error("That plugin is already listed")
            return
        self._commit_plugin_settings()
        self._plugin_names.append(name)
        row = len(self._plugin_names) - 1
        self.plugins_table.insertRow(row)
        item = _item(name)
        item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
        item.setCheckState(Qt.CheckState.Checked)
        self.plugins_table.setItem(row, 0, item)
        self.plugins_table.setCurrentCell(row, 0)
        self.custom_plugin_name.clear()
        self._set_status("Custom plugin added and enabled in memory. Save all to persist it.")

    def _move_plugin(self, offset: int) -> None:
        row = self.plugins_table.currentRow()
        target = row + offset
        if not 0 <= row < len(self._plugin_names) or not 0 <= target < len(self._plugin_names):
            return
        self._gather_plugins()
        checked = [
            self.plugins_table.item(index, 0).checkState() == Qt.CheckState.Checked
            for index in range(len(self._plugin_names))
        ]
        self._plugin_names[row], self._plugin_names[target] = (
            self._plugin_names[target],
            self._plugin_names[row],
        )
        checked[row], checked[target] = checked[target], checked[row]
        with QSignalBlocker(self.plugins_table):
            for index, name in enumerate(self._plugin_names):
                item = _item(name)
                item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
                item.setCheckState(
                    Qt.CheckState.Checked if checked[index] else Qt.CheckState.Unchecked
                )
                self.plugins_table.setItem(index, 0, item)
            self.plugins_table.setCurrentCell(target, 0)
        self._selected_plugin = target
        self._show_plugin_settings(target)

    def save_all(self) -> None:
        try:
            self._gather_core()
            if self.listeners_table.currentRow() >= 0:
                self._update_listener(quiet=True)
            if self.users_table.currentRow() >= 0:
                self._update_user(quiet=True)
            self._gather_plugins()
            self.model.set_config_directory(self.config_path.text())
            self.model.save()
            self._set_status(
                "Configuration files saved using atomic replacement. Restart ADCH++ to apply changes."
            )
        except (ConfigError, OSError, UnicodeError) as exc:
            self._show_error(str(exc))


def run_ui_self_test(configuration: Path) -> None:
    """Construct every page without displaying or modifying the supplied configuration."""
    validation_model = ConfigModel()
    validation_model.load(configuration)
    validation_model.validate()
    validation_model.serialize_core()
    validation_model.serialize_users()
    app = QApplication.instance() or QApplication([])
    window = ConfigWindow(configuration)
    app.processEvents()
    window.close()
    app.processEvents()
