#!/usr/bin/env python3
# Copyright (C) 2026 iceman50
# Licensed under GPL-3.0-or-later.

"""Wire-level smoke tests for the RTF0 and BBS0 plugins.

Build the hub and plugins first, then run this script from the repository root.
An alternate build bin directory may be supplied as the first argument.
"""

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
TTH = "A" * 39
REPLY_TTH = "B" * 39
OVERSIZE_TTH = "C" * 39
UNKNOWN_PARENT_TTH = "D" * 39


class ADCClient:
    def __init__(self, port: int, features: tuple[str, ...]) -> None:
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.socket.settimeout(3)
        self.buffer = b""
        self.send("HSUP " + " ".join("AD" + feature for feature in features))
        sup = self.until(lambda line: line.startswith("ISUP "))
        self.supports = set(part[2:] for part in sup.split()[1:] if part.startswith("AD"))
        sid_line = self.until(lambda line: line.startswith("ISID "))
        self.sid = sid_line.split()[1]
        self.until(lambda line: line.startswith("IINF "))
        self.send("BINF %s NIplugin-test" % self.sid)

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

    def until(self, predicate, limit: int = 40) -> str:
        seen: list[str] = []
        for _ in range(limit):
            line = self.line()
            seen.append(line)
            if predicate(line):
                return line
        raise AssertionError("expected line not received; saw: %r" % seen)


def free_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def write_config(
    directory: Path, port: int, rtf_enabled: bool, max_posts: int = 10
) -> None:
    plugin_path = html.escape(str(BIN) + os.sep, quote=True)
    (directory / "adchpp.xml").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<ADCHubPlusPlus>
  <Settings><Log type="int">0</Log></Settings>
  <Servers><Server Port="{port}" BindAddress="127.0.0.1"/></Servers>
  <Plugins Path="{plugin_path}"><Plugin>RTF0</Plugin><Plugin>BBS0</Plugin></Plugins>
</ADCHubPlusPlus>
""".format(port=port, plugin_path=plugin_path),
        encoding="utf-8",
    )
    (directory / "RTF0.xml").write_text(
        "<RTF0><Enabled>%d</Enabled></RTF0>\n" % (1 if rtf_enabled else 0),
        encoding="utf-8",
    )
    (directory / "BBS0.xml").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<BBS0>
  <Settings Enabled="1" IndexFile="BBS0.index" MaxPostsPerBoard="{max_posts}"
    PostInterval="0" MaxSubscriptions="2" CompactEvery="2"/>
  <Boards>
    <Board Name="general" Title="General" Description="Plugin test"
      MaxSize="4096" ReplayDays="0" Subscribe="guest" Post="guest"
      Reply="guest" WithdrawOwn="guest" WithdrawAny="guest"/>
  </Boards>
</BBS0>
""".format(max_posts=max_posts),
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


def enabled_and_persistence_test(config: Path) -> None:
    port = free_port()
    write_config(config, port, True)
    process = start_hub(config, port)
    try:
        client = ADCClient(port, ("BASE", "TIGR", "RTF0", "BBS0"))
        assert "RTF0" in client.supports
        assert "BBS0" in client.supports
        descriptor = client.until(lambda line: line.startswith("IBBD "))
        assert "BDgeneral" in descriptor and "PE31" in descriptor

        rich = "BMSG %s **rich** RT1" % client.sid
        client.send(rich)
        assert client.until(lambda line: line.startswith("BMSG ")) == rich

        # RTF0 asks receivers to discard the backslash of an undefined ADC
        # escape instead of dropping the complete message.
        client.send("BMSG %s \\*literal RT1" % client.sid)
        recovered = client.until(lambda line: line.startswith("BMSG "))
        assert recovered == "BMSG %s *literal RT1" % client.sid

        client.send("HBBL BDgeneral TS0")
        client.send("HBBP TR%s SI12 BDgeneral SJFirst\\spost" % TTH)
        entry = client.until(lambda line: line.startswith("IBBL ") and " RM1" not in line)
        assert "TR" + TTH in entry and "TH" + TTH in entry and "SJFirst\\spost" in entry

        client.send("HBBP TR%s SI10 BDgeneral PA%s SJReply" % (REPLY_TTH, TTH))
        reply = client.until(
            lambda line: line.startswith("IBBL ") and "TR" + REPLY_TTH in line
        )
        assert "PA" + TTH in reply and "TH" + TTH in reply

        client.send("HBBP TR%s SI10 BDgeneral PA%s" % (REPLY_TTH, TTH))
        duplicate = client.until(lambda line: line.startswith("ISTA 170 "))
        assert "FCBBP" in duplicate and "TR" + REPLY_TTH in duplicate

        client.send("HBBP TR%s SI4097 BDgeneral" % OVERSIZE_TTH)
        oversized = client.until(lambda line: line.startswith("ISTA 172 "))
        assert "MS4096" in oversized and "TR" + OVERSIZE_TTH in oversized

        client.send(
            "HBBP TR%s SI10 BDgeneral PA%s" % (UNKNOWN_PARENT_TTH, OVERSIZE_TTH)
        )
        unknown_parent = client.until(lambda line: line.startswith("ISTA 170 "))
        assert "TR" + UNKNOWN_PARENT_TTH in unknown_parent

        client.send("HBBP TR%s BDgeneral RM1" % TTH)
        tombstone = client.until(lambda line: line.startswith("IBBL ") and " RM1" in line)
        assert "TR" + TTH in tombstone and "BDgeneral" in tombstone
        client.close()
    finally:
        stop_hub(process)

    # A fresh hub process must replay the persisted tombstone.
    process = start_hub(config, port)
    try:
        client = ADCClient(port, ("BASE", "TIGR", "BBS0"))
        client.until(lambda line: line.startswith("IBBD "))
        client.send("HBBL BDgeneral TS0")
        replay = client.until(lambda line: line.startswith("IBBL ") and " RM1" in line)
        assert "TR" + TTH in replay
        client.send("HBBL BDgeneral TR%s" % REPLY_TTH)
        single = client.until(
            lambda line: line.startswith("IBBL ") and "TR" + REPLY_TTH in line
        )
        assert "PA" + TTH in single and "TH" + TTH in single
        client.close()
    finally:
        stop_hub(process)


def disabled_rtf_test(config: Path) -> None:
    port = free_port()
    write_config(config, port, False)
    process = start_hub(config, port)
    try:
        client = ADCClient(port, ("BASE", "TIGR", "RTF0"))
        assert "RTF0" not in client.supports
        client.send("HBBL BDgeneral TS0")
        feature_error = client.until(lambda line: line.startswith("ISTA 145 "))
        assert "FCBBL" in feature_error and "BDgeneral" in feature_error
        client.send("BMSG %s **literal** RT1" % client.sid)
        relayed = client.until(lambda line: line.startswith("BMSG "))
        assert relayed == "BMSG %s **literal**" % client.sid
        client.close()
    finally:
        stop_hub(process)


def retention_test(config: Path) -> None:
    config.mkdir()
    port = free_port()
    write_config(config, port, True, max_posts=1)
    process = start_hub(config, port)
    try:
        client = ADCClient(port, ("BASE", "TIGR", "BBS0"))
        client.until(lambda line: line.startswith("IBBD "))
        old_tth, new_tth = "E" * 39, "F" * 39
        client.send("HBBP TR%s SI10 BDgeneral" % old_tth)
        client.until(lambda line: line.startswith("IBBL ") and "TR" + old_tth in line)
        client.send("HBBP TR%s SI10 BDgeneral" % new_tth)
        client.until(lambda line: line.startswith("IBBL ") and "TR" + new_tth in line)

        client.send("HBBL BDgeneral TR%s" % old_tth)
        missing = client.until(lambda line: line.startswith("ISTA 176 "))
        assert "TR" + old_tth in missing
        client.send("HBBL BDgeneral TR%s" % new_tth)
        retained = client.until(
            lambda line: line.startswith("IBBL ") and "TR" + new_tth in line
        )
        assert "TH" + new_tth in retained
        client.close()
    finally:
        stop_hub(process)

    process = start_hub(config, port)
    try:
        client = ADCClient(port, ("BASE", "TIGR", "BBS0"))
        descriptor = client.until(lambda line: line.startswith("IBBD "))
        assert "NP1" in descriptor and "OT0" not in descriptor
        client.send("HBBL BDgeneral TR%s" % old_tth)
        client.until(lambda line: line.startswith("ISTA 176 "))
        client.send("HBBL BDgeneral TR%s" % new_tth)
        client.until(lambda line: line.startswith("IBBL ") and "TR" + new_tth in line)
        client.close()
    finally:
        stop_hub(process)


def main() -> None:
    if not HUB.exists():
        raise SystemExit("hub executable not found: %s" % HUB)
    for plugin in ("RTF0", "BBS0"):
        suffix = ".dll" if os.name == "nt" else ".so"
        if not (BIN / (plugin + suffix)).exists():
            raise SystemExit("plugin not found: %s" % (BIN / (plugin + suffix)))

    with tempfile.TemporaryDirectory(prefix="adchpp-extension-test-") as temporary:
        config = Path(temporary)
        enabled_and_persistence_test(config)
        disabled_rtf_test(config)
        retention_test(config / "retention")
    print("RTF0/BBS0 wire and persistence tests passed")


if __name__ == "__main__":
    main()
