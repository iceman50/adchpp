#!/usr/bin/env python3
# Copyright (C) 2026 iceman50
# Licensed under GPL-3.0-or-later.

"""Wire-level integration tests for the HBRI plugin."""

from __future__ import annotations

import html
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BIN = ROOT / "build" / "debug-mingw-x64" / "bin"
BIN = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_BIN
HUB = BIN / ("adchppd.exe" if os.name == "nt" else "adchppd")


class ADCConnection:
    def __init__(
        self, host: str, port: int, family: socket.AddressFamily, tls: bool = False
    ) -> None:
        transport = socket.socket(family, socket.SOCK_STREAM)
        transport.settimeout(3)
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            self.socket = context.wrap_socket(transport, server_hostname="localhost")
        else:
            self.socket = transport
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


def assert_no_command(connection: ADCConnection, prefix: str, seconds: float = 0.3) -> None:
    previous_timeout = connection.socket.gettimeout()
    deadline = time.monotonic() + seconds
    try:
        connection.socket.settimeout(seconds)
        while time.monotonic() < deadline:
            try:
                line = connection.line()
            except socket.timeout:
                return
            assert not line.startswith(prefix), "unexpected command: " + line
    finally:
        connection.socket.settimeout(previous_timeout)


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


def write_config(
    directory: Path,
    port: int,
    enabled: bool,
    ipv6_listener: bool = True,
    tls_files: tuple[Path, Path, Path, Path] | None = None,
) -> None:
    plugin_path = html.escape(str(BIN) + os.sep, quote=True)
    log_path = html.escape(str(directory / "adchpp.log"), quote=True)
    listener = (
        '<Server Port="{port}" BindAddress4="127.0.0.1"{ipv6} '
        'HubAddress4="127.0.0.1" HubAddress6="::1"{tls}/>'
    ).format(
        port=port,
        ipv6=' BindAddress6="::1"' if ipv6_listener else "",
        tls=(
            ' TLS="1" Certificate="%s" PrivateKey="%s" TrustedPath="%s" '
            'DHParams="%s" MinVersion="2" SecurityLevel="1"'
            % tuple(html.escape(str(path), quote=True) for path in tls_files)
            if tls_files
            else ""
        ),
    )
    (directory / "adchpp.xml").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<ADCHubPlusPlus>
  <Settings><Log type="int">1</Log><LogFile type="string">{log_path}</LogFile></Settings>
  <Servers>
    {listener}
  </Servers>
  <Plugins Path="{plugin_path}"><Plugin>HBRI</Plugin></Plugins>
</ADCHubPlusPlus>
""".format(listener=listener, plugin_path=plugin_path, log_path=log_path),
        encoding="utf-8",
    )
    (directory / "HBRI.xml").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<HBRI><Settings Enabled="{enabled}" Address4="" Address6="" Port="{port}"/></HBRI>
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
    process.wait(timeout=5)
    output = process.stdout.read() if process.stdout else ""
    log_file = config / "adchpp.log"
    if log_file.exists():
        output += "\n" + log_file.read_text(encoding="utf-8", errors="replace")
    config_file = config / "adchpp.xml"
    if config_file.exists():
        output += "\nConfiguration:\n" + config_file.read_text(
            encoding="utf-8", errors="replace"
        )
    raise AssertionError("hub did not start listening:\n" + output)


def stop_hub(process: subprocess.Popen[str]) -> None:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def validate(
    challenge: str,
    family: socket.AddressFamily,
    port: int,
    udp: str = "",
    tls: bool = False,
) -> ADCConnection:
    token = named(challenge, "TO")
    assert token and len(token) == 39
    host = "::1" if family == socket.AF_INET6 else "127.0.0.1"
    validation = ADCConnection(host, port, family, tls=tls)
    validation.send("HTCP TO%s%s" % (token, (" " + udp) if udp else ""))
    return validation


def enabled_test(config: Path) -> None:
    port = free_port()
    write_config(config, port, True)
    process = start_hub(config, port)
    try:
        main = ADCConnection("127.0.0.1", port, socket.AF_INET)
        sid, supports = main.handshake("hbri-test", "I6:: U612345")
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

        # A malformed or wrong-family value is stripped but is not treated as
        # HBRI intent and therefore cannot make the hub issue a challenge.
        main.send("BINF %s I6127.0.0.1 U612399" % sid)
        assert_no_command(main, "ITCP ")

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

        # Tokens have a short lifetime and a timed-out attempt can be retried
        # with a fresh, independently generated token.
        main.send("BINF %s I6:: U612349" % sid)
        expired_challenge = main.until(lambda line: line.startswith("ITCP "))
        time.sleep(10.1)
        expired = validate(expired_challenge, socket.AF_INET6, port)
        assert expired.until(lambda line: line.startswith("ISTA ")).startswith("ISTA 150 ")
        expired.close()
        main.send("BINF %s I6:: U612350" % sid)
        retry_challenge = main.until(lambda line: line.startswith("ITCP "))
        retry = validate(retry_challenge, socket.AF_INET6, port)
        assert retry.until(lambda line: line.startswith("ISTA ")).startswith("ISTA 000 ")
        retry.close()
        retry_update = main.until(
            lambda line: line.startswith("BINF " + sid + " ")
            and named(line, "I6") == "::1"
            and named(line, "U6") == "12350"
        )
        assert retry_update

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
            "hbri-v6-test", "I40.0.0.0 U412347"
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


def listener_gating_test(config: Path) -> None:
    port = free_port()
    write_config(config, port, True, ipv6_listener=False)
    process = start_hub(config, port)
    try:
        client = ADCConnection("127.0.0.1", port, socket.AF_INET)
        _, supports = client.handshake("hbri-one-family")
        assert "HBRI" not in supports
        client.close()
    finally:
        stop_hub(process)


def dns_resolution_test(config: Path) -> None:
    port = free_port()
    write_config(config, port, True)
    (config / "HBRI.xml").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<HBRI><Settings Enabled="1" Address4="localhost"
  Address6="localhost" Port="{port}"/></HBRI>
""".format(port=port),
        encoding="utf-8",
    )
    process = start_hub(config, port)
    try:
        main = ADCConnection("127.0.0.1", port, socket.AF_INET)
        sid, supports = main.handshake("hbri-dns", "I6:: U612351")
        assert "HBRI" in supports
        main.until(lambda line: line.startswith("BINF " + sid + " "))
        challenge = main.until(lambda line: line.startswith("ITCP "))
        assert named(challenge, "I6") == "::1"
        validation = validate(challenge, socket.AF_INET6, port)
        assert validation.until(lambda line: line.startswith("ISTA ")).startswith("ISTA 000 ")
        validation.close()
        main.close()
    finally:
        stop_hub(process)


def generate_tls_files(directory: Path) -> tuple[Path, Path, Path, Path] | None:
    openssl = shutil.which("openssl")
    if not openssl:
        print("HBRI TLS test skipped: openssl was not found")
        return None

    certificate = directory / "cacert.pem"
    private_key = directory / "privkey.pem"
    dh_parameters = directory / "dhparam.pem"
    trusted = directory / "trusted"
    trusted.mkdir()
    commands = [
        [
            openssl,
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-sha256",
            "-nodes",
            "-days",
            "1",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1",
            "-keyout",
            str(private_key),
            "-out",
            str(certificate),
        ],
        [
            openssl,
            "genpkey",
            "-genparam",
            "-algorithm",
            "DH",
            "-pkeyopt",
            "group:ffdhe2048",
            "-out",
            str(dh_parameters),
        ],
    ]
    for command in commands:
        subprocess.run(command, check=True, capture_output=True, text=True)
    return certificate, private_key, trusted, dh_parameters


def tls_test(config: Path) -> None:
    tls_files = generate_tls_files(config)
    if not tls_files:
        return
    port = free_port()
    write_config(config, port, True, tls_files=tls_files)
    process = start_hub(config, port)
    try:
        # A plaintext client on the TLS listener must be rejected cleanly
        # without poisoning the accept loop or retaining a login object.
        malformed = socket.create_connection(("127.0.0.1", port), timeout=1)
        malformed.sendall(b"not-a-tls-handshake\n")
        malformed.close()

        main = ADCConnection("127.0.0.1", port, socket.AF_INET, tls=True)
        sid, supports = main.handshake("hbri-tls", "I6:: U612348")
        assert "HBRI" in supports
        main.until(lambda line: line.startswith("BINF " + sid + " "))
        challenge = main.until(lambda line: line.startswith("ITCP "))
        validation = validate(challenge, socket.AF_INET6, port, tls=True)
        assert validation.until(lambda line: line.startswith("ISTA ")).startswith("ISTA 000 ")
        validation.close()
        update = main.until(
            lambda line: line.startswith("BINF " + sid + " ")
            and named(line, "I6") == "::1"
        )
        assert named(update, "U6") == "12348"
        main.close()
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
        one_family = root / "one-family"
        one_family.mkdir()
        listener_gating_test(one_family)
        dns = root / "dns"
        dns.mkdir()
        dns_resolution_test(dns)
        tls = root / "tls"
        tls.mkdir()
        tls_test(tls)
    print("HBRI wire tests passed")


if __name__ == "__main__":
    main()
