#!/usr/bin/env python3
"""Build, sign, and manually upload one VIEWE OTA release.

Examples:
  python3 tools/ota.py meter1
  python3 tools/ota.py meter1 meter2 meter3
  python3 tools/ota.py --host 192.168.4.1
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys


PROJECT_DIR = Path(__file__).resolve().parents[1]


def run(command):
    try:
        subprocess.run(command, cwd=PROJECT_DIR, check=True)
    except FileNotFoundError:
        sys.exit("Required command was not found: {}".format(command[0]))
    except subprocess.CalledProcessError as error:
        sys.exit("Command failed (exit {}): {}".format(error.returncode, " ".join(map(str, command))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", nargs="*", help="mDNS name(s), with or without .local")
    parser.add_argument("--host", action="append", default=[], help="IP address, hostname, or http:// URL; repeatable")
    parser.add_argument("--version", help="optional version override (normally use the generated build version)")
    parser.add_argument("-e", "--environment", default="viewe",
                        help="PlatformIO environment to build (default: viewe)")
    parser.add_argument("--no-wait", action="store_true", help="do not wait for the device to reboot")
    args = parser.parse_args()
    if not args.device and not args.host:
        parser.error("specify at least one device name or --host")

    pio = shutil.which("pio")
    if not pio:
        sys.exit("PlatformIO `pio` was not found in PATH.")

    # `latest` is a local, ignored staging directory; versioned releases can
    # still be created with `pio run -t release` when they need to be retained.
    release_dir = PROJECT_DIR / "dist" / "latest"
    run([pio, "run", "-e", args.environment])

    try:
        ota_header = (PROJECT_DIR / "include" / "ota_public_key.h").read_text(encoding="utf-8")
        board_match = re.search(r'^#define OTA_BOARD_ID "([^"]+)"$', ota_header, re.MULTILINE)
        ota_board = board_match.group(1) if board_match else None
    except OSError:
        ota_board = None
    if not ota_board:
        sys.exit("Generated OTA board identity was not found after the build.")

    release_command = [sys.executable, "tools/release.py", "--firmware",
                       ".pio/build/{}/firmware.bin".format(args.environment),
                       "--output", str(release_dir), "--board", ota_board]
    if args.version:
        release_command.extend(["--version", args.version])
    run(release_command)

    # Upload the one signed artifact to every requested device. No additional
    # PlatformIO builds occur inside this loop, so all meters receive the same
    # generated version and binary.
    for device in args.device:
        upload_command = [sys.executable, "tools/ota_upload.py", "--release", str(release_dir), "--device", device]
        if args.no_wait:
            upload_command.extend(["--wait", "0"])
        run(upload_command)
    for host in args.host:
        upload_command = [sys.executable, "tools/ota_upload.py", "--release", str(release_dir), "--host", host]
        if args.no_wait:
            upload_command.extend(["--wait", "0"])
        run(upload_command)


if __name__ == "__main__":
    main()
