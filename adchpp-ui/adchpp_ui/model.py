"""Read, validate, and atomically write ADCH++ configuration files."""

from __future__ import annotations

import copy
import json
import os
import re
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


class ConfigError(ValueError):
    """A configuration could not be loaded or validated."""


@dataclass
class CoreSettings:
    hub_name: str = "ADCH++"
    description: str = "ADCH++ hub"
    log_enabled: bool = True
    log_file: str = "logs/adchpp%Y%m.log"
    max_command_size: str = "32768"
    buffer_size: str = "4096"
    max_buffer_size: str = "65536"
    overflow_timeout: str = "60000"
    overflow_limit: str = "25"
    disconnect_timeout: str = "10000"
    log_timeout: str = "10000"


@dataclass
class ListenerSettings:
    port: str = "2780"
    bind_address: str = ""
    bind_address4: str = ""
    bind_address6: str = ""
    hub_address4: str = ""
    hub_address6: str = ""
    tls: bool = False
    certificate: str = ""
    private_key: str = ""
    trusted_path: str = ""
    dh_params: str = ""
    min_version: str = "2"
    security_level: str = "1"
    cipher_suites13: str = ""


CORE_XML_FIELDS = (
    ("hub_name", "HubName", "string"),
    ("description", "Description", "string"),
    ("log_file", "LogFile", "string"),
    ("max_command_size", "MaxCommandSize", "int"),
    ("buffer_size", "BufferSize", "int"),
    ("max_buffer_size", "MaxBufferSize", "int"),
    ("overflow_timeout", "OverflowTimeout", "int"),
    ("overflow_limit", "OverflowLimit", "unsignedByte"),
    ("disconnect_timeout", "DisconnectTimeout", "int"),
    ("log_timeout", "LogTimeout", "int"),
)

LISTENER_ATTRIBUTES = (
    ("port", "Port", True),
    ("bind_address", "BindAddress", False),
    ("bind_address4", "BindAddress4", False),
    ("bind_address6", "BindAddress6", False),
    ("hub_address4", "HubAddress4", False),
    ("hub_address6", "HubAddress6", False),
    ("certificate", "Certificate", False),
    ("private_key", "PrivateKey", False),
    ("trusted_path", "TrustedPath", False),
    ("dh_params", "DHParams", False),
    ("min_version", "MinVersion", False),
    ("security_level", "SecurityLevel", False),
    ("cipher_suites13", "CipherSuites1_3", False),
)

SAFE_PLUGIN_NAME = re.compile(r"^[A-Za-z0-9_.-]+$")
BASE32_CID = re.compile(r"^[A-Z2-7]{39}$")


def _xml_parser() -> ET.XMLParser:
    return ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))


def _parse_xml(text: str, description: str) -> ET.Element:
    if not text.strip():
        raise ConfigError(f"{description} must not be empty")
    try:
        return ET.fromstring(text, parser=_xml_parser())
    except (ET.ParseError, ValueError) as exc:
        raise ConfigError(f"{description} is not valid XML: {exc}") from exc


def _reject_json_constant(value: str) -> None:
    raise ConfigError(f"users.txt contains the non-standard value {value}")


def _read_text(path: Path, limit: int = 64 * 1024 * 1024) -> str:
    try:
        if path.stat().st_size > limit:
            raise ConfigError(f"Configuration file exceeds the 64 MiB safety limit: {path}")
        return path.read_text(encoding="utf-8-sig")
    except ConfigError:
        raise
    except (OSError, UnicodeError) as exc:
        raise ConfigError(f"Unable to read {path}: {exc}") from exc


def _write_atomic(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        if path.exists():
            os.chmod(temporary, path.stat().st_mode)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.close(descriptor)
        except OSError:
            pass
        temporary.unlink(missing_ok=True)
        raise


def _find_child(parent: ET.Element, tag: str) -> ET.Element | None:
    return next((child for child in parent if child.tag == tag), None)


def _ensure_child(parent: ET.Element, tag: str) -> ET.Element:
    child = _find_child(parent, tag)
    if child is None:
        child = ET.SubElement(parent, tag)
    return child


def _integer(value: Any, label: str, minimum: int, maximum: int) -> int:
    text = str(value)
    if not re.fullmatch(r"-?(0|[1-9][0-9]*)", text):
        raise ConfigError(f"{label} must be a whole number")
    number = int(text)
    if not minimum <= number <= maximum:
        raise ConfigError(f"{label} must be between {minimum} and {maximum}")
    return number


def is_safe_plugin_name(name: str) -> bool:
    return bool(name and name not in {".", ".."} and SAFE_PLUGIN_NAME.fullmatch(name))


class ConfigModel:
    """Editable representation of one ADCH++ configuration directory."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.config_directory = Path()
        self._directory_selected = False
        self.settings = CoreSettings()
        self.listeners = [ListenerSettings()]
        self.plugin_path = ""
        self.plugins: list[str] = []
        self.users: list[dict[str, Any]] = []
        self.plugin_settings: dict[str, str] = {}
        self._core_root = ET.Element("ADCHubPlusPlus")
        ET.SubElement(self._core_root, "Settings")
        ET.SubElement(self._core_root, "Servers")
        ET.SubElement(self._core_root, "Plugins")

    def load(self, directory: str | os.PathLike[str]) -> bool:
        self.reset()
        self.set_config_directory(directory)

        core_file = self.config_directory / "adchpp.xml"
        existing = core_file.is_file()
        if existing:
            self._load_core(_read_text(core_file))

        users_file = self.config_directory / "users.txt"
        if users_file.is_file():
            self._load_users(_read_text(users_file))

        candidates = list(self.plugins)
        if self.config_directory.is_dir():
            for path in sorted(self.config_directory.glob("*.xml")):
                if path.name.lower() == "adchpp.xml":
                    continue
                if is_safe_plugin_name(path.stem) and path.stem not in candidates:
                    candidates.append(path.stem)
        for name in candidates:
            if not is_safe_plugin_name(name):
                continue
            path = self.config_directory / f"{name}.xml"
            if path.is_file():
                self.plugin_settings[name] = _read_text(path)
        return existing

    def set_config_directory(self, directory: str | os.PathLike[str]) -> None:
        if not str(directory).strip():
            raise ConfigError("Select a configuration directory")
        resolved = Path(directory).expanduser().resolve(strict=False)
        if resolved.exists() and not resolved.is_dir():
            raise ConfigError("The configuration path points to a file")
        self.config_directory = resolved
        self._directory_selected = True

    def _load_core(self, text: str) -> None:
        root = _parse_xml(text, "adchpp.xml")
        if root.tag != "ADCHubPlusPlus":
            raise ConfigError("adchpp.xml has no ADCHubPlusPlus root element")
        self._core_root = root

        settings = _find_child(root, "Settings")
        if settings is not None:
            for attribute, tag, _value_type in CORE_XML_FIELDS:
                element = _find_child(settings, tag)
                if element is not None:
                    setattr(self.settings, attribute, element.text or "")
            log = _find_child(settings, "Log")
            if log is not None:
                self.settings.log_enabled = (log.text or "") == "1"

        servers = _find_child(root, "Servers")
        if servers is not None:
            self.listeners = []
            for element in servers:
                if element.tag != "Server":
                    continue
                listener = ListenerSettings()
                for attribute, xml_name, _required in LISTENER_ATTRIBUTES:
                    default = getattr(listener, attribute)
                    setattr(listener, attribute, element.get(xml_name, default))
                listener.tls = element.get("TLS", "0") == "1"
                self.listeners.append(listener)

        plugins = _find_child(root, "Plugins")
        if plugins is not None:
            self.plugin_path = plugins.get("Path", "")
            self.plugins = [
                child.text or "" for child in plugins if child.tag == "Plugin"
            ]

    def _load_users(self, text: str) -> None:
        try:
            users = json.loads(text, parse_constant=_reject_json_constant)
        except ConfigError:
            raise
        except (json.JSONDecodeError, TypeError) as exc:
            raise ConfigError(f"users.txt is not valid JSON: {exc}") from exc
        if not isinstance(users, list) or any(not isinstance(user, dict) for user in users):
            raise ConfigError("users.txt must contain an array of user objects")
        self.users = users

    def configured_plugin_names(self) -> list[str]:
        result = list(self.plugins)
        for name in self.plugin_settings:
            if name not in result:
                result.append(name)
        return result

    def discover_plugin_names(self) -> list[str]:
        directories: list[Path] = []
        if self.plugin_path:
            configured = Path(self.plugin_path)
            if not configured.is_absolute():
                configured = self.config_directory.parent / configured
            directories.append(configured)
        directories.extend((self.config_directory.parent, Path.cwd()))

        suffixes = {".dll", ".so", ".dylib"}
        result: list[str] = []
        for directory in directories:
            try:
                files: Iterable[Path] = directory.iterdir()
            except OSError:
                continue
            for path in files:
                if path.is_file() and path.suffix.lower() in suffixes:
                    name = path.stem
                    if is_safe_plugin_name(name) and name not in result:
                        result.append(name)
        return sorted(result, key=str.casefold)

    def validate(self) -> None:
        if not self._directory_selected:
            raise ConfigError("Select a configuration directory")
        if not self.settings.hub_name:
            raise ConfigError("Hub name must not be empty")
        _integer(self.settings.max_command_size, "Maximum command size", 0, 2**31 - 1)
        buffer_size = _integer(self.settings.buffer_size, "Buffer size", 256, 2**31 - 1)
        max_buffer = _integer(
            self.settings.max_buffer_size, "Maximum buffer size", 256, 2**31 - 1
        )
        if max_buffer < buffer_size:
            raise ConfigError("Maximum buffer size must not be smaller than buffer size")
        _integer(self.settings.overflow_timeout, "Overflow timeout", 0, 2**31 - 1)
        _integer(self.settings.overflow_limit, "Overflow limit", 0, 100)
        _integer(self.settings.disconnect_timeout, "Disconnect timeout", 0, 2**31 - 1)
        _integer(self.settings.log_timeout, "Login timeout", 0, 2**31 - 1)

        if not self.listeners:
            raise ConfigError("At least one listener is required")
        for index, listener in enumerate(self.listeners, 1):
            prefix = f"Listener {index}"
            _integer(listener.port, f"{prefix} port", 1, 65535)
            _integer(listener.min_version, f"{prefix} minimum TLS version", 0, 3)
            _integer(listener.security_level, f"{prefix} TLS security level", 0, 5)
            if listener.tls and (not listener.certificate or not listener.private_key):
                raise ConfigError(
                    f"{prefix} requires a certificate and private key when TLS is enabled"
                )

        if self.plugin_path and not self.plugin_path.endswith(("/", "\\")):
            raise ConfigError("Plugin DLL path must end with a slash or backslash")
        if any(not name for name in self.plugins):
            raise ConfigError("Plugin names must not be empty")
        if len(set(self.plugins)) != len(self.plugins):
            raise ConfigError("Loaded plugin names must not be duplicated")

        nicknames: set[str] = set()
        cids: set[str] = set()
        for index, user in enumerate(self.users, 1):
            if not isinstance(user, dict):
                raise ConfigError(f"Registered user {index} is not an object")
            nick = user.get("nick")
            password = user.get("password")
            level = user.get("level")
            cid = user.get("cid", "")
            if not isinstance(nick, str) or not nick:
                raise ConfigError(f"Registered user {index} has no nickname")
            if any(character.isspace() for character in nick):
                raise ConfigError(f"Registered nickname {nick} contains whitespace")
            if not isinstance(password, str) or not password:
                raise ConfigError(f"Registered user {nick} has no password")
            if isinstance(level, bool) or not isinstance(level, int):
                raise ConfigError(f"Level for {nick} must be a whole number")
            _integer(level, f"Level for {nick}", 1, 2**31 - 1)
            if nick in nicknames:
                raise ConfigError(f"Registered nickname {nick} is duplicated")
            nicknames.add(nick)
            if cid:
                if not isinstance(cid, str) or not BASE32_CID.fullmatch(cid):
                    raise ConfigError(
                        f"CID for {nick} must contain exactly 39 uppercase base32 characters"
                    )
                if cid in cids:
                    raise ConfigError(f"CID for {nick} is duplicated")
                cids.add(cid)

        try:
            json.dumps(self.users, allow_nan=False)
        except (TypeError, ValueError) as exc:
            raise ConfigError(f"Registered-user metadata is not valid JSON: {exc}") from exc

        for name, xml_text in self.plugin_settings.items():
            if not is_safe_plugin_name(name):
                raise ConfigError(f"Unsafe plugin settings name: {name}")
            _parse_xml(xml_text, f"{name}.xml")

    @staticmethod
    def _replace_elements(
        parent: ET.Element, tag: str, replacements: list[ET.Element]
    ) -> None:
        children = list(parent)
        positions = [index for index, child in enumerate(children) if child.tag == tag]
        insert_at = positions[0] if positions else len(children)
        for child in children:
            if child.tag == tag:
                parent.remove(child)
        for offset, element in enumerate(replacements):
            parent.insert(insert_at + offset, element)

    def serialize_core(self) -> str:
        root = copy.deepcopy(self._core_root)
        settings = _ensure_child(root, "Settings")
        for attribute, tag, value_type in CORE_XML_FIELDS:
            element = _ensure_child(settings, tag)
            element.set("type", value_type)
            element.text = str(getattr(self.settings, attribute))
        log = _ensure_child(settings, "Log")
        log.set("type", "int")
        log.text = "1" if self.settings.log_enabled else "0"

        servers = _ensure_child(root, "Servers")
        server_elements: list[ET.Element] = []
        for listener in self.listeners:
            element = ET.Element("Server")
            for attribute, xml_name, required in LISTENER_ATTRIBUTES:
                value = str(getattr(listener, attribute))
                if required or value:
                    element.set(xml_name, value)
            if listener.tls:
                element.set("TLS", "1")
            server_elements.append(element)
        self._replace_elements(servers, "Server", server_elements)

        plugins = _ensure_child(root, "Plugins")
        if self.plugin_path:
            plugins.set("Path", self.plugin_path)
        else:
            plugins.attrib.pop("Path", None)
        plugin_elements = []
        for name in self.plugins:
            element = ET.Element("Plugin")
            element.text = name
            plugin_elements.append(element)
        self._replace_elements(plugins, "Plugin", plugin_elements)

        ET.indent(root, space="\t")
        body = ET.tostring(root, encoding="unicode", short_empty_elements=True)
        return '<?xml version="1.0" encoding="utf-8" standalone="yes"?>\n' + body + "\n"

    def serialize_users(self) -> str:
        return json.dumps(
            self.users, ensure_ascii=False, allow_nan=False, indent=2, sort_keys=True
        ) + "\n"

    def save(self) -> None:
        self.validate()
        core_xml = self.serialize_core()
        users_json = self.serialize_users()
        _parse_xml(core_xml, "Generated adchpp.xml")
        self.config_directory.mkdir(parents=True, exist_ok=True)
        _write_atomic(self.config_directory / "adchpp.xml", core_xml)
        _write_atomic(self.config_directory / "users.txt", users_json)
        for name, xml_text in self.plugin_settings.items():
            _write_atomic(self.config_directory / f"{name}.xml", xml_text)


def run_model_self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="adchpp-ui-") as temporary:
        directory = Path(temporary)
        (directory / "users.txt").write_text(
            '[{"nick":"bad","level":1,"future":{"accepted":no}}]', encoding="utf-8"
        )
        try:
            ConfigModel().load(directory)
        except ConfigError:
            pass
        else:
            raise AssertionError("Malformed nested users JSON was accepted")
        (directory / "users.txt").unlink()

        model = ConfigModel()
        assert not model.load(directory)
        model.settings.hub_name = "UI & model test"
        model.settings.description = "UTF-8: ✓"
        model.plugins = ["Script"]
        model.plugin_settings["Script"] = "<ScriptPlugin>"
        try:
            model.validate()
        except ConfigError:
            pass
        else:
            raise AssertionError("Malformed plugin XML was accepted")
        model.plugin_settings["Script"] = (
            '<?xml version="1.0"?><ScriptPlugin><Engine language="lua" '
            'scriptPath="Scripts/"><Script>access.lua</Script></Engine></ScriptPlugin>\n'
        )
        model.users.append(
            {
                "nick": "owner",
                "password": 'quote"slash\\test',
                "level": 10,
                "regby": "owner",
                "regtime": 1,
                "future": {"kept": True},
            }
        )
        model.save()

        loaded = ConfigModel()
        assert loaded.load(directory)
        assert loaded.settings.hub_name == model.settings.hub_name
        assert loaded.settings.description == model.settings.description
        assert loaded.users[0]["future"] == {"kept": True}
        assert loaded.plugin_settings["Script"] == model.plugin_settings["Script"]
