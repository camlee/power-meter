# Signed OTA updates

The firmware accepts an application image only when all of these checks pass:

- the canonical manifest has a valid ECDSA P-256/SHA-256 signature;
- the signed board identity matches `meter-viewe` or `meter-wroom`;
- the streamed image hash and exact byte count match the manifest; and
- the version is newer than the installed/highest confirmed version.

LAN uploads and Internet downloads use the same verifier and inactive-slot
writer. The private signing key is the update authorization boundary; there is
no compiled shared bearer token.

## One-time setup

On a trusted build machine:

```sh
cd esp32-viewe
cp .env.example .env
python3 tools/create_ota_keys.py
```

Keep `.env` and `secrets/ota_signing_private.pem` off the device and out of
Git. The public key in `keys/ota_signing_public.pem` is committed and embedded
in the firmware. Every existing device needs one USB or compatible signed
update containing the current OTA verifier; older bootloaders/partition tables
also require USB flashing.

Do not rotate the key casually. A planned rotation needs an intermediate image
that trusts both keys; a compromised key requires trusted USB recovery.

## Local updates

Build, sign, upload, reboot, and check a device with:

```sh
python3 tools/ota.py meter1
python3 tools/ota.py --host 192.168.4.1
python3 tools/ota.py -e wroom meter-wroom-1
```

Use `--host` when mDNS is unavailable. The tool can upload several devices
from one build. A previously signed release can be reused with:

```sh
python3 tools/ota_upload.py --device meter2 --release dist/latest
```

Local OTA intentionally rejects equal or older versions; USB is the downgrade
and recovery path.

## Internet releases

Stable releases are built and published with one version argument:

```sh
python3 tools/publish_github_release.py 0.1.2
```

The command runs the release checks, builds and signs both targets, creates the
version tag, pushes the current branch/tag, and creates a draft GitHub release.
Review and publish the draft. `--dry-run` prints the resolved release plan;
`--skip-tests` is for recovery only.

Devices consume a board-specific descriptor from the configured public GitHub
release repository. They verify the descriptor and signed manifest before
constructing the fixed asset URL. Internet updates wait for a confirmed image,
network connectivity, and credible time; stable automatic updates can be
enabled from the Info screen. Failed downloads are discarded, and an image
that resets before confirmation rolls back and is blocked from automatic retry.

## Recovery boundary

The inactive slot is not selected until signature, size, and hash checks pass.
The new image must run healthily for its confirmation window before becoming
the active rollback target.

Use USB for intentional downgrades, bootloader/partition changes, LittleFS
changes, signing-key changes, or devices that cannot boot or join the LAN.
This project does not enable Secure Boot or flash encryption; physical
flash-write access is outside this OTA threat model.
