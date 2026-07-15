#!/usr/bin/env python3
"""Exercise the ECDSA P-256 OTA key, release, and detached signature tools."""

import base64
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


PROJECT_DIR = Path(__file__).resolve().parents[1]


class OtaCryptoTest(unittest.TestCase):
    def test_generated_release_verifies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private_key = root / "private.pem"
            public_key = root / "public.pem"
            firmware = root / "firmware.bin"
            release = root / "release"
            firmware.write_bytes(bytes(range(256)) * 4)

            subprocess.run([
                sys.executable, str(PROJECT_DIR / "tools" / "create_ota_keys.py"),
                "--private-key", str(private_key), "--public-key", str(public_key),
            ], check=True)
            subprocess.run([
                sys.executable, str(PROJECT_DIR / "tools" / "release.py"),
                "--firmware", str(firmware), "--private-key", str(private_key),
                "--output", str(release), "--version", "crypto-test",
            ], check=True)

            signature = base64.b64decode((release / "manifest.sig").read_text(encoding="ascii"))
            self.assertLessEqual(len(signature), 80)
            signature_path = release / "manifest.sig.bin"
            signature_path.write_bytes(signature)
            manifest = json.loads((release / "manifest.json").read_text(encoding="ascii"))
            self.assertEqual(manifest["image_size"], firmware.stat().st_size)
            subprocess.run([
                "openssl", "dgst", "-sha256", "-verify", str(public_key),
                "-signature", str(signature_path),
                str(release / "manifest.json"),
            ], check=True)


if __name__ == "__main__":
    unittest.main()
