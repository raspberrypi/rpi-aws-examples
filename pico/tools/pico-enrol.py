#!/usr/bin/env python3
"""Register a Pico running hello-mqtt with AWS IoT, over its USB serial port.

    pico/tools/pico-enrol.py --port /dev/ttyACM0 [--region eu-west-2] [--ssid NETWORK]
        asks for the Wi-Fi network and password if the Pico has none (or --ssid is given), reads the Pico's
        CSR, runs tools/aws-iot-register with it, and writes the certificate and endpoint back to the Pico,
        which checks them against its key, saves them and restarts.

Or in steps, when the Pico is plugged into another computer than the one with your AWS credentials:

    pico-enrol.py csr --port /dev/ttyACM0 > pico.csr          (where the Pico is)
    tools/aws-iot-register --csr pico.csr                       (where your AWS credentials are)
    pico-enrol.py install --port /dev/ttyACM0 --dir rpi-ex-...  (where the Pico is, with that directory)
    pico-enrol.py wifi --port /dev/ttyACM0 [--ssid NETWORK]     (where the Pico is: set or change Wi-Fi)

Only the CSR leaves the Pico; its private key never does. The Wi-Fi password is asked for without echo and
goes only to the Pico, which keeps it wrapped by the key ladder on a Pico 2 W. Needs Python 3 only.
"""

import argparse
import getpass
import json
import os
import select
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent.parent / "tools"


class Pico:
    def __init__(self, port: str):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[2] |= termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""
        # The Pico sends nothing until it sees the port open; give it a moment, then drop what came before.
        time.sleep(1.0)
        os.write(self.fd, b"\n")
        while self.line(0.5) is not None:
            pass
        self.buf = b""

    def line(self, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                return None
            self.buf += os.read(self.fd, 4096)
        raw, self.buf = self.buf.split(b"\n", 1)
        return raw.decode(errors="replace").rstrip("\r")

    def command(self, text: str, timeout: float = 10.0, expect_ok: bool = True) -> list[str]:
        """Send one command; return the lines of its answer, up to OK. Other output (logs) is skipped."""
        os.write(self.fd, text.encode() + b"\n")
        if not expect_ok:
            return []
        lines: list[str] = []
        while True:
            line = self.line(timeout)
            if line is None:
                sys.exit(f"error: no answer from the Pico to {text.split()[0]} (is hello-mqtt running?)")
            if line == "OK":
                return lines
            if line.startswith("ERROR"):
                sys.exit(f"error: the Pico says {line}")
            lines.append(line)


def read_csr(pico: Pico) -> tuple[str, str]:
    thing = [x for x in pico.command("ID") if x.startswith("rpi-ex-")][-1]
    lines = pico.command("CSR", timeout=30)
    start = lines.index("-----BEGIN CERTIFICATE REQUEST-----")
    return thing, "\n".join(lines[start:]) + "\n"


def install(pico: Pico, directory: Path) -> None:
    config = json.loads((directory / "config.json").read_text())
    cert = (directory / "device.crt").read_text().strip().splitlines()
    pico.command(f"ENDPOINT {config['endpoint']}")
    os.write(pico.fd, b"CERT\n")
    for line in cert[:-1]:
        os.write(pico.fd, line.encode() + b"\n")
        time.sleep(0.005)
    pico.command(cert[-1])
    pico.command("SAVE", timeout=30)
    print(f"Saved on the Pico: {config['thingName']} at {config['endpoint']}. It restarts, and connects once it "
          "has Wi-Fi settings (pico-enrol.py wifi).", file=sys.stderr)


def wifi_ssid(pico: Pico) -> str | None:
    line = [x for x in pico.command("WIFI-STATUS") if x.startswith("wifi: ")][-1]
    return None if line == "wifi: none" else line[6:]


def set_wifi(pico: Pico, ssid: str | None) -> None:
    ssid = ssid or input("Wi-Fi network (SSID): ").strip()
    if not 1 <= len(ssid.encode()) <= 32:
        sys.exit("error: an SSID is 1 to 32 bytes")
    password = getpass.getpass(f"Password for {ssid} (not shown): ")
    if not (8 <= len(password) <= 63 or (len(password) == 64 and all(c in "0123456789abcdefABCDEF" for c in password))):
        sys.exit("error: a WPA2 password is 8 to 63 characters, or 64 hex digits")
    pico.command(f"WIFI-SSID {ssid}")
    pico.command(f"WIFI-PASSWORD {password}")
    pico.command("WIFI-SAVE", timeout=30)
    print(f"Wi-Fi saved on the Pico: {ssid}", file=sys.stderr)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("step", nargs="?", choices=["csr", "install", "wifi"], help="one step only (default: all)")
    p.add_argument("--port", required=True, help="the Pico's USB serial port, for example /dev/ttyACM0")
    p.add_argument("--dir", type=Path, help="install: the directory aws-iot-register wrote")
    p.add_argument("--region")
    p.add_argument("--ssid", help="the Wi-Fi network to set (the password is asked for)")
    args = p.parse_args()
    pico = Pico(args.port)

    if args.step == "csr":
        thing, csr = read_csr(pico)
        print(f"CSR for {thing}", file=sys.stderr)
        sys.stdout.write(csr)
        return 0
    if args.step == "install":
        if not args.dir:
            p.error("install needs --dir")
        install(pico, args.dir)
        return 0
    if args.step == "wifi":
        set_wifi(pico, args.ssid)
        return 0

    if args.ssid or wifi_ssid(pico) is None:
        set_wifi(pico, args.ssid)
    thing, csr = read_csr(pico)
    csr_file = Path(f"{thing}.csr")
    csr_file.write_text(csr)
    register = [str(TOOLS / "aws-iot-register"), "--csr", str(csr_file)] + (
        ["--region", args.region] if args.region else [])
    subprocess.run(register, check=True, stdout=subprocess.DEVNULL)
    install(pico, Path(thing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
