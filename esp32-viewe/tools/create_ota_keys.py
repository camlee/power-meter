#!/usr/bin/env python3
"""Create the local RSA key pair used to sign VIEWE OTA releases."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


PROJECT_DIR = Path(__file__).resolve().parents[1]


def run(command):
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        sys.exit("openssl was not found. Install OpenSSL and try again.")
    except subprocess.CalledProcessError as error:
        sys.exit("OpenSSL failed with exit code {}.".format(error.returncode))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--private-key",
        type=Path,
        default=PROJECT_DIR / "secrets" / "ota_signing_private.pem",
    )
    parser.add_argument(
        "--public-key",
        type=Path,
        default=PROJECT_DIR / "keys" / "ota_signing_public.pem",
    )
    parser.add_argument("--force", action="store_true", help="replace existing key files")
    args = parser.parse_args()

    if not args.force and (args.private_key.exists() or args.public_key.exists()):
        sys.exit(
            "Refusing to overwrite an existing key. Use --force only when rotating "
            "the key and reflashing every device."
        )

    args.private_key.parent.mkdir(parents=True, exist_ok=True)
    args.public_key.parent.mkdir(parents=True, exist_ok=True)
    run([
        "openssl", "genpkey", "-algorithm", "RSA",
        "-pkeyopt", "rsa_keygen_bits:3072",
        "-out", str(args.private_key),
    ])
    os.chmod(args.private_key, 0o600)
    run([
        "openssl", "pkey", "-in", str(args.private_key), "-pubout",
        "-out", str(args.public_key),
    ])
    print("Created private key: {}".format(args.private_key))
    print("Created public key:  {}".format(args.public_key))
    print("Keep the private key out of Git and back it up securely.")


if __name__ == "__main__":
    main()
