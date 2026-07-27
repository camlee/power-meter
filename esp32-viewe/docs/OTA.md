# Signed local and Internet OTA updates

Power-meter devices accept an application image only when:

- its canonical manifest has a valid ECDSA P-256/SHA-256 signature;
- the signed board is `meter-viewe` or `meter-wroom` and matches the device;
- the streamed firmware SHA-256 and exact byte count match the manifest; and
- its Semantic Versioning precedence is newer than the installed/highest
  confirmed version.

Both LAN uploads and automatic GitHub downloads use the same verifier and
inactive-slot writer. The signing private key is the update authorization
boundary. There is no compiled shared bearer token.

## One-time setup

Install PlatformIO, Python 3, and OpenSSL, then create the signing key:

```sh
cd esp32-viewe
cp .env.example .env
python3 tools/create_ota_keys.py
```

`secrets/ota_signing_private.pem` must stay on trusted build machines and have
a secure backup. `keys/ota_signing_public.pem` is intentionally committed and
embedded in firmware.

Every existing device needs one USB or signed LAN update containing the
Internet updater before it can update itself. A device with an older bootloader
also needs one USB flash so rollback support and the current partition table
are installed:

```sh
pio run -e viewe -t upload
```

Do not rotate the key casually. A planned rotation requires an intermediate
firmware that trusts both old and new public keys; recovery from a compromised
key requires a trusted USB flash.

## Local signed updates

Build, sign, upload, reboot, and confirm one or more devices:

```sh
python3 tools/ota.py meter1 meter2
python3 tools/ota.py --host 192.168.4.1
python3 tools/ota.py -e wroom meter-wroom-1
```

The uploader resolves `.local` names itself and falls back to the system
resolver. Use the displayed/serial IP when multicast DNS is unavailable.

A development build derives a short SemVer-compatible identity from the nearest
`v*` tag using `git describe`, such as `0.1.0-3+g1a2b3c4`. Local OTA only
accepts a newer version; USB is the intentional downgrade/recovery path.

Create or upload a retained local release:

```sh
pio run -e viewe -t release
python3 tools/ota_upload.py --device meter1 --release dist/<version>
```

The local release contains `firmware.bin`, `manifest.json`, and
`manifest.sig`. `POST /api/v1/update` is reachable on the trusted LAN but will
write only an image authorized by the embedded public key.

## Internet update behavior

The device checks this small board-specific descriptor:

```text
https://github.com/camlee/power-meter/releases/latest/download/ota-meter-viewe.json
```

The descriptor carries base64 of the exact signed manifest and its detached
signature. After verification, the device constructs a version-specific
firmware URL from the signed release tag and fixed asset name.

Automatic behavior:

- wait for a confirmed running image, Internet connectivity, and credible
  network time before HTTPS;
- on Internet acquisition, check after one to five minutes only when due;
- check every 24 hours after the last successful check;
- retry failures after roughly 5 minutes, 15 minutes, 1 hour, then 6 hours;
- install stable releases automatically when the setting is enabled;
- never retry a version that caused rollback; and
- allow Check now and Install controls from both Info screens.

The HTTPS client uses the ESP certificate bundle, verifies hostnames, limits
redirects, and streams directly into the inactive OTA slot. A failed or
interrupted transfer is discarded and restarted from byte zero later.

`GET /api/v1/updates` reports the state. The command endpoints are:

```text
POST /api/v1/updates/check
POST /api/v1/updates/install
PUT  /api/v1/updates/settings   {"automatic":true}
```

## Build GitHub release assets

Stable release identity comes from one tag-supplied `MAJOR.MINOR.PATCH`
version, ensuring both hardware builds contain the same version.

Commit the intended release, then:

```sh
git tag -a v0.1.0 -m "Power meter v0.1.0"
git push origin main
git push origin v0.1.0

python3 tools/build_github_release.py 0.1.0
```

The command builds both PlatformIO environments, signs both manifests with the
local private key, explicitly suppresses developer `.env` AP credentials from
the public binaries, and creates:

```text
dist/v0.1.0/
  firmware-meter-viewe.bin
  firmware-meter-wroom.bin
  manifest-meter-viewe.json
  manifest-meter-viewe.sig
  manifest-meter-wroom.json
  manifest-meter-wroom.sig
  ota-meter-viewe.json
  ota-meter-wroom.json
```

The `manifest-*` files are retained for inspection; devices download only the
small `ota-*` descriptor and their matching firmware asset.

## GitHub publication steps

The configured repository is `camlee/power-meter`. It must be public for
token-free device downloads. If the source must remain private, create a
dedicated public binary-release repository and change
`custom_ota_release_repo` in both PlatformIO environments before the first
Internet-capable device build.

1. In GitHub, open **Settings → General → Releases** and enable release
   immutability. It applies only to future releases.
2. Install/authenticate GitHub CLI if desired: `gh auth login`.
3. Create a draft and upload every generated asset:

   ```sh
   gh release create v0.1.0 --draft --verify-tag \
     --title v0.1.0 dist/v0.1.0/*
   ```

4. In the GitHub draft, confirm all eight assets are present, add release
   notes, and publish it as the latest release.
5. Run `python3 tools/test_ota_crypto.py`, then download the published assets
   once and compare them with the files you built:

   ```sh
   mkdir -p /tmp/power-meter-v0.1.0
   gh release download v0.1.0 --dir /tmp/power-meter-v0.1.0
   diff <(cd dist/v0.1.0 && sha256sum *) \
        <(cd /tmp/power-meter-v0.1.0 && sha256sum *)
   ```

6. Use **Check now** on the first device and observe download, reboot,
   confirmation, and network recovery before allowing the second device to
   update.

Drafts and prereleases are not consumed by the stable updater.

## Failure and recovery

The inactive slot is not selected until signature, size, and hash checks have
all passed. A new image remains pending for 30 seconds of normal main-loop
operation. A crash/reset before confirmation rolls back to the prior image.
The failed version is persisted and blocked from automatic retry.

Use Settings → Debug, `/api/v1/debug`, serial logs, or `/api/v1/info` to inspect
health, slots, image state, and rollback. USB remains the recovery route for:

- an intentional downgrade;
- bootloader or partition changes;
- LittleFS images;
- signing-key changes; or
- a device that cannot boot or join the LAN.

This project does not enable hardware Secure Boot or flash encryption. A party
with physical flash-write access remains outside this OTA threat model.
