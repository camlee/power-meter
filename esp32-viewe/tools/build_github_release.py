#!/usr/bin/env python3
"""Build and sign the two immutable GitHub release asset sets."""

import argparse
import datetime
import os
from pathlib import Path
import re
import subprocess
import sys


PROJECT_DIR = Path(__file__).resolve().parents[1]
STABLE_SEMVER = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
TARGETS = (
    ("viewe", "meter-viewe"),
    ("wroom", "meter-wroom"),
)


def run(command, **kwargs):
    print("+", " ".join(str(value) for value in command))
    subprocess.run(command, cwd=PROJECT_DIR, check=True, **kwargs)


def require_tag(version):
    tag = "v" + version
    try:
        tagged = subprocess.check_output(
            ["git", "rev-parse", "{}^{{commit}}".format(tag)],
            cwd=PROJECT_DIR, text=True, stderr=subprocess.DEVNULL).strip()
        head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=PROJECT_DIR, text=True).strip()
    except subprocess.CalledProcessError:
        sys.exit("Tag {} does not exist. Create and push the release tag first.".format(tag))
    if tagged != head:
        sys.exit("Tag {} does not point to the current commit.".format(tag))
    return tag


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="stable MAJOR.MINOR.PATCH release version")
    parser.add_argument(
        "--private-key", type=Path,
        default=PROJECT_DIR / "secrets" / "ota_signing_private.pem",
    )
    args = parser.parse_args()
    if not STABLE_SEMVER.fullmatch(args.version):
        sys.exit("Version must be stable MAJOR.MINOR.PATCH without a leading v.")
    if not args.private_key.is_file():
        sys.exit("OTA signing key not found: {}".format(args.private_key))
    tag = require_tag(args.version)
    try:
        source_epoch = subprocess.check_output(
            ["git", "show", "-s", "--format=%ct", "HEAD"],
            cwd=PROJECT_DIR, text=True).strip()
    except subprocess.CalledProcessError:
        source_epoch = str(int(datetime.datetime.now(datetime.timezone.utc).timestamp()))

    output = PROJECT_DIR / "dist" / tag
    if output.exists() and any(output.iterdir()):
        sys.exit("Release output already exists and is not empty: {}".format(output))
    output.mkdir(parents=True, exist_ok=True)

    build_environment = os.environ.copy()
    build_environment["VIEWE_VERSION"] = args.version
    build_environment["SOURCE_DATE_EPOCH"] = source_epoch
    # Public firmware must never inherit a developer's local AP defaults.
    build_environment["VIEWE_PUBLIC_RELEASE"] = "1"
    for environment, board in TARGETS:
        run(["pio", "run", "-e", environment], env=build_environment)
        firmware = PROJECT_DIR / ".pio" / "build" / environment / "firmware.bin"
        run([
            sys.executable, "tools/release.py",
            "--internet",
            "--firmware", str(firmware),
            "--private-key", str(args.private_key),
            "--output", str(output),
            "--version", args.version,
            "--board", board,
        ], env=build_environment)

    assets = sorted(path.name for path in output.iterdir())
    print("\nCreated GitHub release assets in {}".format(output))
    for asset in assets:
        print("  {}".format(asset))
    print("\nAfter reviewing the assets, create a draft release with:")
    print("  gh release create {} --draft --verify-tag --title {} {}/*".format(
        tag, tag, output))


if __name__ == "__main__":
    main()
