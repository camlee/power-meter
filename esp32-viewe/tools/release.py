#!/usr/bin/env python3
"""Produce a signed, detached-manifest OTA release from a PlatformIO binary."""

import argparse
import base64
import datetime
import hashlib
import json
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


def default_board():
    try:
        header = (PROJECT_DIR / "include" / "ota_public_key.h").read_text(encoding="utf-8")
        match = re.search(r'^#define OTA_BOARD_ID "([^"]+)"$', header, re.MULTILINE)
        if match:
            return match.group(1)
    except OSError:
        pass
    return "meter-viewe"


def sign(payload, private_key):
    inspect = ["openssl", "pkey", "-in", str(private_key), "-text", "-noout"]
    try:
        key_info = subprocess.run(inspect, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, check=True).stdout
    except FileNotFoundError:
        sys.exit("openssl was not found. Install OpenSSL and try again.")
    except subprocess.CalledProcessError as error:
        sys.exit("OpenSSL could not read the signing key (exit {}).".format(error.returncode))
    if "ASN1 OID: prime256v1" not in key_info and "NIST CURVE: P-256" not in key_info:
        sys.exit("Signing key must be ECDSA P-256. Rotate it with tools/create_ota_keys.py --force.")
    command = [
        "openssl", "pkeyutl", "-sign", "-rawin", "-digest", "sha256",
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
    parser.add_argument("--board", default=default_board())
    parser.add_argument(
        "--private-key", type=Path,
        default=PROJECT_DIR / "secrets" / "ota_signing_private.pem",
    )
    parser.add_argument("--output", type=Path, help="release directory (default: dist/<version>)")
    parser.add_argument(
        "--internet", action="store_true",
        help="create fixed-name GitHub release assets and a signed descriptor",
    )
    args = parser.parse_args()

    if not args.firmware.is_file():
        sys.exit("Firmware binary not found: {}".format(args.firmware))
    if not args.private_key.is_file():
        sys.exit("Signing key not found: {}\nRun tools/create_ota_keys.py first.".format(args.private_key))
    if not args.version or "/" in args.version or "\\" in args.version:
        sys.exit("Version must be a non-empty name without path separators.")

    output = args.output or PROJECT_DIR / "dist" / args.version
    output.mkdir(parents=True, exist_ok=True)
    firmware_name = "firmware-{}.bin".format(args.board) if args.internet else "firmware.bin"
    firmware_output = output / firmware_name
    if args.firmware.resolve() != firmware_output.resolve():
        shutil.copyfile(args.firmware, firmware_output)

    image = firmware_output.read_bytes()
    if args.internet:
        if not re.fullmatch(
                r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)",
                args.version):
            sys.exit("Internet releases must use a stable MAJOR.MINOR.PATCH version.")
        manifest = {
            "board": args.board,
            "channel": "stable",
            "firmware_asset": firmware_name,
            "format": 2,
            "image_size": len(image),
            "release_tag": "v" + args.version,
            "sha256": hashlib.sha256(image).hexdigest(),
            "version": args.version,
        }
    else:
        manifest = {
            "board": args.board,
            "format": 1,
            "image_size": len(image),
            "sha256": hashlib.sha256(image).hexdigest(),
            "version": args.version,
        }
    payload = canonical_json(manifest)
    signature = sign(payload, args.private_key)
    if not signature or len(signature) > 80:
        sys.exit("OpenSSL produced an invalid ECDSA P-256 signature.")
    manifest_name = "manifest-{}.json".format(args.board) if args.internet else "manifest.json"
    signature_name = "manifest-{}.sig".format(args.board) if args.internet else "manifest.sig"
    encoded_signature = base64.b64encode(signature).decode("ascii")
    (output / manifest_name).write_bytes(payload)
    (output / signature_name).write_text(encoded_signature + "\n", encoding="ascii")
    if args.internet:
        descriptor = canonical_json({
            "manifest_b64": base64.b64encode(payload).decode("ascii"),
            "signature_b64": encoded_signature,
        })
        (output / "ota-{}.json".format(args.board)).write_bytes(descriptor)

    print("Created signed OTA release: {}".format(output))
    print("  firmware: {} bytes".format(len(image)))
    print("  SHA-256:  {}".format(manifest["sha256"]))


if __name__ == "__main__":
    main()
