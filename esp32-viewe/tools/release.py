#!/usr/bin/env python3
"""Produce a signed, detached-manifest OTA release from a PlatformIO binary."""

import argparse
import base64
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import re


PROJECT_DIR = Path(__file__).resolve().parents[1]


def canonical_json(value):
    """The byte representation covered by the detached signature."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def default_version():
    try:
        header = (PROJECT_DIR / "include" / "build_time.h").read_text(encoding="utf-8")
        match = re.search(r'^#define BUILD_VERSION "([^"]+)"$', header, re.MULTILINE)
        if match:
            return match.group(1)
    except OSError:
        pass
    try:
        return subprocess.check_output(
            ["git", "-c", "safe.directory={}".format(PROJECT_DIR.parent),
             "describe", "--always", "--dirty"],
            cwd=str(PROJECT_DIR), text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d%H%M%S")


def sign(payload, private_key):
    command = [
        "openssl", "pkeyutl", "-sign", "-rawin", "-digest", "sha256",
        "-pkeyopt", "rsa_padding_mode:pss",
        "-pkeyopt", "rsa_pss_saltlen:digest",
        "-inkey", str(private_key),
    ]
    try:
        completed = subprocess.run(command, input=payload, stdout=subprocess.PIPE, check=True)
    except FileNotFoundError:
        sys.exit("openssl was not found. Install OpenSSL and try again.")
    except subprocess.CalledProcessError as error:
        sys.exit("OpenSSL could not sign the manifest (exit {}).".format(error.returncode))
    return completed.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", required=True, type=Path, help="compiled firmware.bin")
    parser.add_argument("--version", default=default_version())
    parser.add_argument("--board", default=os.environ.get("VIEWE_OTA_BOARD", "meter"))
    parser.add_argument(
        "--private-key", type=Path,
        default=PROJECT_DIR / "secrets" / "ota_signing_private.pem",
    )
    parser.add_argument("--output", type=Path, help="release directory (default: dist/<version>)")
    args = parser.parse_args()

    if not args.firmware.is_file():
        sys.exit("Firmware binary not found: {}".format(args.firmware))
    if not args.private_key.is_file():
        sys.exit("Signing key not found: {}\nRun tools/create_ota_keys.py first.".format(args.private_key))
    if not args.version or "/" in args.version or "\\" in args.version:
        sys.exit("Version must be a non-empty name without path separators.")

    output = args.output or PROJECT_DIR / "dist" / args.version
    output.mkdir(parents=True, exist_ok=True)
    firmware_name = "firmware.bin"
    firmware_output = output / firmware_name
    if args.firmware.resolve() != firmware_output.resolve():
        shutil.copyfile(args.firmware, firmware_output)

    image = firmware_output.read_bytes()
    manifest = {
        "board": args.board,
        "format": 1,
        "image_size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "version": args.version,
    }
    payload = canonical_json(manifest)
    signature = sign(payload, args.private_key)
    (output / "manifest.json").write_bytes(payload)
    (output / "manifest.sig").write_text(base64.b64encode(signature).decode("ascii") + "\n", encoding="ascii")

    print("Created signed OTA release: {}".format(output))
    print("  firmware: {} bytes".format(len(image)))
    print("  SHA-256:  {}".format(manifest["sha256"]))


if __name__ == "__main__":
    main()
