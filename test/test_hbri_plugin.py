#!/usr/bin/env python3
# Copyright (C) 2026 iceman50
# Licensed under GPL-3.0-or-later.

"""Wire-level integration tests for the HBRI plugin."""

from __future__ import annotations

import html
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BIN = ROOT / "build" / "debug-mingw-x64" / "bin"
BIN = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_BIN
HUB = BIN / ("adchppd.exe" if os.name == "nt" else "adchppd")


class ADCConnection:
    def __init__(self, host: str, port: int, family: socket.AddressFamily) -> None:
        self.socket = socket.socket(family, socket.SOCK_STREAM)
        self.socket.settimeout(3)
        address = (host, port, 0, 0) if family == socket.AF_INET6 else (host, port)
        self.socket.connect(address)
        self.buffer = b""

    def close(self) -> None:
        self.socket.close()

    def send(self, line: str) -> None:
        self.socket.sendall((line + "\n").encode("utf-8"))

    def line(self) -> str:
        while b"\n" not in self.buffer:
            block = self.socket.recv(65536)
            if not block:
                raise AssertionError("hub closed the connection")
            self.buffer += block
        raw, self.buffer = self.buffer.split(b"\n", 1)
        return raw.decode("utf-8")

    def until(self, predicate, limit: int = 50) -> str:
        seen: list[str] = []
        for _ in range(limit):
            line = self.line()
            seen.append(line)
            if predicate(line):
                return line
        raise AssertionError("expected line not received; saw: %r" % seen)

    def handshake(self, nick: str, secondary: str | None = None) -> tuple[str, set[str]]:
        self.send("HSUP ADBASE ADTIGR ADHBRI")
        sup = self.until(lambda line: line.startswith("ISUP "))
        supports = {part[2:] for part in sup.split()[1:] if part.startswith("AD")}
        sid = self.until(lambda line: line.startswith("ISID ")).split()[1]
        self.until(lambda line: line.startswith("IINF "))
        params = " NI" + nick
        if secondary:
            params += " " + secondary
        self.send("BINF " + sid + params)
        return sid, supports


def named(line: str, name: str) -> str | None:
    for parameter in line.split()[1:]:
        if parameter.startswith(name):
            return parameter[2:]
    return None


def free_port() -> int:
    ipv6 = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    ipv6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
    ipv6.bind(("::1", 0))
    port = ipv6.getsockname()[1]
    ipv6.close()

    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("127.0.0.1", port))
    except OSError:
        probe.close()
        return free_port()
    probe.close()
    return port


def write_config(directory: Path, port: int, enabled: bool) -> None:
    plugin_path = html.escape(str(BIN) + os.sep, quote=True)
    (directory / "adchpp.xml").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<ADCHubPlusPlus>
  <Settings><Log type="int">0</Log></Settings>
  <Servers>
    <Server Port="{port}" BindAddress="127.0.0.1"/>
    <Server Port="{port}" BindAddress="::1"/>
  </Servers>
  <Plugins Path="{plugin_path}"><Plugin>HBRI</Plugin></Plugins>
</ADCHubPlusPlus>
""".format(port=port, plugin_path=plugin_path),
        encoding="utf-8",
    )
    (directory / "HBRI.xml").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<HBRI><Settings Enabled="{enabled}" Address4="127.0.0.1"
  Address6="::1" Port="{port}"/></HBRI>
""".format(enabled=1 if enabled else 0, port=port),
        encoding="utf-8",
    )


def start_hub(config: Path, port: int) -> subprocess.Popen[str]:
    process = subprocess.Popen(
        [str(HUB), "-c", str(config)],
        cwd=str(BIN),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            output = process.stdout.read() if process.stdout else ""
            raise AssertionError("hub exited during startup:\n" + output)
        try:
            probe = socket.create_connection(("127.0.0.1", port), timeout=0.1)
            probe.close()
            return process
        except OSError:
            time.sleep(0.05)
    process.terminate()
    raise AssertionError("hub did not start listening")


def stop_hub(process: subprocess.Popen[str]) -> None:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def validate(challenge: str, family: socket.AddressFamily, port: int, udp: str = "") -> ADCConnection:
    token = named(challenge, "TO")
    assert token and len(token) == 39
    host = "::1" if family == socket.AF_INET6 else "127.0.0.1"
    validation = ADCConnection(host, port, family)
    validation.send("HTCP TO%s%s" % (token, (" " + udp) if udp else ""))
    return validation


def enabled_test(config: Path) -> None:
    port = free_port()
    write_config(config, port, True)
    process = start_hub(config, port)
    try:
        main = ADCConnection("127.0.0.1", port, socket.AF_INET)
        sid, supports = main.handshake("hbri-test", "I6::1 U612345")
        assert "HBRI" in supports

        initial = main.until(lambda line: line.startswith("BINF " + sid + " "))
        assert named(initial, "I4") == "127.0.0.1"
        assert named(initial, "I6") is None
        assert named(initial, "U6") is None

        challenge = main.until(lambda line: line.startswith("ITCP "))
        assert named(challenge, "I6") == "::1"
        assert named(challenge, "P6") == str(port)

        validation = validate(challenge, socket.AF_INET6, port)
        assert validation.until(lambda line: line.startswith("ISTA ")).startswith("ISTA 000 ")
        validation.close()

        update = main.until(
            lambda line: line.startswith("BINF " + sid + " ") and named(line, "I6") is not None
        )
        assert named(update, "I6") == "::1"
        assert named(update, "U6") == "12345"

        # Validation tokens are single-use even while the main session stays
        # connected, so replay cannot overwrite an already validated address.
        replay = validate(challenge, socket.AF_INET6, port)
        assert replay.until(lambda line: line.startswith("ISTA ")).startswith("ISTA 150 ")
        replay.close()

        # A changed secondary address triggers a new challenge. The hub still
        # publishes the address observed on the validation socket and restores
        # the newly advertised UDP port when HTCP omits it.
        main.send("BINF %s I62001:db8::1 U612346" % sid)
        challenge = main.until(lambda line: line.startswith("ITCP "))
        validation = validate(challenge, socket.AF_INET6, port)
        assert validation.until(lambda line: line.startswith("ISTA 000 "))
        validation.close()
        update = main.until(
            lambda line: line.startswith("BINF " + sid + " ")
            and named(line, "I6") == "::1"
            and named(line, "U6") == "12346"
        )
        assert update

        # A token cannot be validated over the same family as the main link.
        main.send("BINF %s I62001:db8::2" % sid)
        challenge = main.until(lambda line: line.startswith("ITCP "))
        wrong = validate(challenge, socket.AF_INET, port)
        assert wrong.until(lambda line: line.startswith("ISTA ")).startswith("ISTA 151 ")
        wrong.close()

        # The reverse direction is equally important: an IPv6 main connection
        # must be able to prove its IPv4 endpoint.
        main_v6 = ADCConnection("::1", port, socket.AF_INET6)
        sid_v6, supports_v6 = main_v6.handshake(
            "hbri-v6-test", "I4127.0.0.1 U412347"
        )
        assert "HBRI" in supports_v6
        initial_v6 = main_v6.until(lambda line: line.startswith("BINF " + sid_v6 + " "))
        assert named(initial_v6, "I6") == "::1"
        assert named(initial_v6, "I4") is None
        assert named(initial_v6, "U4") is None
        challenge_v4 = main_v6.until(lambda line: line.startswith("ITCP "))
        assert named(challenge_v4, "I4") == "127.0.0.1"
        assert named(challenge_v4, "P4") == str(port)
        validation_v4 = validate(challenge_v4, socket.AF_INET, port, "U412347")
        assert validation_v4.until(lambda line: line.startswith("ISTA 000 "))
        validation_v4.close()
        update_v4 = main_v6.until(
            lambda line: line.startswith("BINF " + sid_v6 + " ")
            and named(line, "I4") == "127.0.0.1"
        )
        assert named(update_v4, "U4") == "12347"
        main_v6.close()
        main.close()
    finally:
        stop_hub(process)


def disabled_test(config: Path) -> None:
    port = free_port()
    write_config(config, port, False)
    process = start_hub(config, port)
    try:
        client = ADCConnection("127.0.0.1", port, socket.AF_INET)
        _, supports = client.handshake("hbri-disabled")
        assert "HBRI" not in supports
        client.close()
    finally:
        stop_hub(process)


def main() -> None:
    if not HUB.exists():
        raise SystemExit("hub executable not found: %s" % HUB)
    suffix = ".dll" if os.name == "nt" else ".so"
    if not (BIN / ("HBRI" + suffix)).exists():
        raise SystemExit("plugin not found: %s" % (BIN / ("HBRI" + suffix)))

    with tempfile.TemporaryDirectory(prefix="adchpp-hbri-test-") as temporary:
        root = Path(temporary)
        enabled_test(root)
        disabled = root / "disabled"
        disabled.mkdir()
        disabled_test(disabled)
    print("HBRI wire tests passed")


if __name__ == "__main__":
    main()
