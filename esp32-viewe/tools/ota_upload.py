#!/usr/bin/env python3
"""Upload a previously signed VIEWE OTA release to one device."""

import argparse
import base64
import json
import mimetypes
import os
from pathlib import Path
import secrets
import sys
import time
import urllib.error
import urllib.request

from mdns_resolver import normalize_hostname, resolve_ipv4


PROJECT_DIR = Path(__file__).resolve().parents[1]


def load_env(path):
    values = {}
    if not path.is_file():
        return values
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def multipart(fields, files):
    boundary = "----viewe-" + secrets.token_hex(16)
    body = bytearray()
    for name, value in fields.items():
        body.extend(("--{}\r\n".format(boundary)).encode("ascii"))
        body.extend(("Content-Disposition: form-data; name=\"{}\"\r\n\r\n".format(name)).encode("utf-8"))
        body.extend(value.encode("utf-8"))
        body.extend(b"\r\n")
    for name, path, content_type in files:
        body.extend(("--{}\r\n".format(boundary)).encode("ascii"))
        body.extend((
            "Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n"
            "Content-Type: {}\r\n\r\n".format(name, path.name, content_type)
        ).encode("utf-8"))
        body.extend(path.read_bytes())
        body.extend(b"\r\n")
    body.extend(("--{}--\r\n".format(boundary)).encode("ascii"))
    return bytes(body), "multipart/form-data; boundary={}".format(boundary)


def request(url, token, data=None, content_type=None, timeout=15):
    headers = {"Authorization": "Bearer " + token}
    if content_type:
        headers["Content-Type"] = content_type
    return urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers), timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--host", help="IP address or hostname, optionally including http://")
    target.add_argument("--device", help="mDNS device name, with or without .local")
    parser.add_argument("--release", type=Path, help="directory containing firmware.bin, manifest.json, and manifest.sig")
    parser.add_argument("--env-file", type=Path, default=PROJECT_DIR / ".env")
    parser.add_argument("--token", help="override VIEWE_OTA_TOKEN from .env")
    parser.add_argument("--timeout", type=float, default=30, help="upload request timeout in seconds")
    parser.add_argument("--wait", type=float, default=45, help="seconds to wait for device reboot and image confirmation; 0 disables")
    args = parser.parse_args()

    if not args.host and not args.device:
        args.host = os.environ.get("VIEWE_OTA_HOST")
        args.device = os.environ.get("VIEWE_OTA_DEVICE")
    if bool(args.host) == bool(args.device):
        sys.exit("Specify exactly one of --host/--device (or VIEWE_OTA_HOST/VIEWE_OTA_DEVICE).")
    release = args.release or (Path(os.environ["VIEWE_OTA_RELEASE"]) if os.environ.get("VIEWE_OTA_RELEASE") else PROJECT_DIR / "dist")
    firmware = release / "firmware.bin"
    manifest = release / "manifest.json"
    signature = release / "manifest.sig"
    for path in (firmware, manifest, signature):
        if not path.is_file():
            sys.exit("Release file not found: {}".format(path))
    token = args.token or os.environ.get("VIEWE_OTA_TOKEN") or load_env(args.env_file).get("VIEWE_OTA_TOKEN")
    if not token:
        sys.exit("VIEWE_OTA_TOKEN is required; set it in {} or pass --token.".format(args.env_file))

    host = args.host or args.device
    if args.device:
        host = normalize_hostname(host)
        addresses = resolve_ipv4(host)
        if addresses:
            print("Resolved {} to {} with direct mDNS.".format(host, addresses[0]))
            host = addresses[0]
        else:
            print("Direct mDNS did not resolve {}; trying the system resolver.".format(host))
    base_url = host.rstrip("/") if "://" in host else "http://" + host.rstrip("/")
    fields = {
        "manifest": manifest.read_text(encoding="ascii"),
        "signature": signature.read_text(encoding="ascii").strip(),
    }
    expected = json.loads(fields["manifest"])
    body, content_type = multipart(fields, [
        ("firmware", firmware, "application/octet-stream"),
    ])
    url = base_url + "/api/v1/update"
    print("Uploading {} to {} …".format(firmware, url))
    try:
        with request(url, token, body, content_type, args.timeout) as response:
            reply = response.read().decode("utf-8", errors="replace")
            print("Device response (HTTP {}): {}".format(response.status, reply or "accepted"))
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        sys.exit("Update rejected (HTTP {}): {}".format(error.code, detail))
    except urllib.error.URLError as error:
        sys.exit("Could not reach {}: {}".format(base_url, error.reason))

    if args.wait <= 0:
        return
    print("Waiting for reboot", end="", flush=True)
    deadline = time.monotonic() + args.wait
    while time.monotonic() < deadline:
        time.sleep(1)
        try:
            with request(base_url + "/api/v1/info", token, timeout=2) as response:
                info = json.loads(response.read().decode("utf-8", errors="strict"))
                if info.get("board") != expected["board"] or info.get("version") != expected["version"]:
                    print(".", end="", flush=True)
                    continue
                if info.get("rollback_detected") or info.get("health") == "rolled_back":
                    sys.exit("\nDevice rolled back instead of confirming version {}.".format(expected["version"]))
                if info.get("health") == "confirmed" and info.get("validated") is True:
                    print("\nDevice confirmed version {} on {}.".format(info["version"], info.get("running_partition", "unknown slot")))
                    return
                print(".", end="", flush=True)
        except (urllib.error.URLError, urllib.error.HTTPError):
            print(".", end="", flush=True)
    sys.exit("\nTimed out waiting for the new firmware to confirm. Check its display or serial log.")


if __name__ == "__main__":
    main()
