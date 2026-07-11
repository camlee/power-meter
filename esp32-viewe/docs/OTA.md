# Manual signed OTA updates

VIEWE devices accept updates only when all of these conditions are met:

- the request has the shared bearer token;
- `manifest.json` has a valid detached RSA-3072/RSA-PSS/SHA-256 signature;
- the firmware SHA-256 and size match the signed manifest; and
- the manifest board is `meter`.

This is deliberately a direct, local workflow: devices do not check in, poll a
server, or require an Internet connection. The updater talks to one device at a
time over the same network.

## One-time setup

Install PlatformIO, Python 3, and OpenSSL on the build machine. Create the
local secrets and the public verification key:

```sh
cd esp32-viewe
cp .env.example .env
openssl rand -hex 32
# Put that output after VIEWE_OTA_TOKEN= in .env.
python3 tools/create_ota_keys.py
```

`secrets/ota_signing_private.pem` is the release key. It is ignored by Git and
must remain only on trusted build machines (with a secure backup).
`keys/ota_signing_public.pem` contains no secret; firmware embeds it to verify
releases. Create the key before the first wired flash so every device trusts
that public key. The PlatformIO pre-build script copies the `.env` token and
public key into the ignored `include/ota_public_key.h` header; it is what makes
the same shared token available to each device flashed from this checkout.

Never use `--force` on `create_ota_keys.py` casually. Rotating the signing key
requires a wired flash of every device with firmware that trusts the new public
key.

## Provision a new device

Use USB for an initial flash or recovery:

```sh
pio run -t upload
```

Set a unique device ID on its display. The default format is `meter-xxxxxxxx`.
On Wi-Fi it advertises `<device-id>.local`; the display also shows its current
DHCP address. When it is operating its own setup access point, connect to that
AP and use its gateway address (normally `192.168.4.1`). mDNS is convenient
same-LAN discovery, not required infrastructure; multicast-isolated networks
may require the displayed IP address instead.

## Create and install a release

For the usual one-device update, use the short command below. It builds,
signs, and uploads a temporary local release in one step:

```sh
python3 tools/ota.py meter1
```

`meter1` resolves as `meter1.local`. To update through the meter's access
point instead, use `python3 tools/ota.py --host 192.168.4.1`. The following
commands remain useful when you want to retain an explicitly versioned release
or debug one stage of the process.

Each build receives a local, auto-incrementing `0.0.N` version. One command
builds once, signs once, and can update several meters with that same version:

```sh
python3 tools/ota.py meter1 meter2 meter3
```

To retain a named release directory, build and sign it separately:

```sh
pio run -t release
```

This builds the normal PlatformIO application and writes these files to
`dist/0.0.N/`, using that build's generated version:

- `firmware.bin` — the inactive OTA slot image to upload;
- `manifest.json` — canonical, sorted-key JSON with `format`, `board`,
  `version`, `sha256`, and `image_size`;
- `manifest.sig` — base64 detached RSA-PSS/SHA-256 signature over the exact
  bytes of `manifest.json`.

The build number is stored locally in ignored `.build_number`, so it does not
dirty Git and the next PlatformIO build advances it. A release can also be made
directly from an existing binary:

```sh
python3 tools/release.py --firmware .pio/build/meter/firmware.bin
```

Upload one release manually. Both target forms are supported:

```sh
python3 tools/ota_upload.py --device meter-jyiu4k32 --release dist/0.0.N
python3 tools/ota_upload.py --host 192.168.4.1 --release dist/0.0.N

# Equivalent PlatformIO target (the target performs an upload; it does not build).
VIEWE_OTA_HOST=meter-jyiu4k32.local VIEWE_OTA_RELEASE=dist/0.0.N pio run -t ota
```

`--device` adds `.local` when needed. `--host` may be an IP address, hostname,
or complete `http://` URL. The uploader reads `VIEWE_OTA_TOKEN` from `.env`,
POSTs `multipart/form-data` to `/api/v1/update`, and sends:

- `Authorization: Bearer <shared token>`;
- `manifest` as a text form field containing the exact canonical JSON bytes;
- `signature` as base64 text; and
- `firmware` as `application/octet-stream`.

It then polls `/api/v1/info` for a short time after the device reboots. An
accepted update can be installed regardless of whether its version is lower or
higher than the currently running firmware.

## Failure and recovery

The device writes to the inactive OTA application partition. It does not choose
that partition for the next boot until signature, manifest, and written-image
hash verification complete. If upload or verification fails, the currently
running application remains selected. If the device cannot return after an
accepted update, inspect the display/serial log and use USB recovery:

```sh
pio run -t upload
```

USB upload is also required if the partition table, bootloader, LittleFS image,
public signing key, or shared token changes. Manual OTA updates only carry the
application `firmware.bin`.

## Security boundary

The shared bearer token is a lightweight LAN control to prevent accidental or
casual update requests. It is compiled into each device and is therefore not a
strong secret against a party that can extract flash contents. The signing
private key is the important authorization boundary: someone with only the
token cannot make a device boot modified firmware.

The updater uses HTTP to keep setup suitable for small, changing local
networks. The signature prevents a network observer from modifying an image,
but HTTP does not prevent denial of service or reveal the shared token to an
active network observer. Use a trusted LAN/AP; HTTPS can be added later if that
threat model changes. This project intentionally does not enable ESP secure
boot or burn eFuses.
