# ESP32 VIEWE Power Meter Display

PlatformIO Arduino project for an ESP32-S3 VIEWE display module using ESP Display Panel and [LVGL](https://lvgl.io/docs/open) v8.

## Board Settings

- Board: ESP32S3 Dev Module
- USB CDC On Boot: Disabled
- USB DFU On Boot: Disabled
- Flash: 16MB
- Partition scheme: 3MB OTA app slots with LittleFS data partition
- PSRAM: OPI PSRAM
- Monitor speed: 115200

The active ESP Display Panel board is configured in `include/esp/esp_panel_board_supported_conf.h`:

```cpp
#define BOARD_VIEWE_UEDX32480035E_WB_A
```

## Updating firmware

Firmware versions are generated as
`0.0.N` and increment once per build. A single build can therefore update many
meters with the exact same signed firmware.

### USB / hardwired update

Connect one meter over USB, then run:

```sh
pio run -t upload
```

To watch logs:

```sh
screen /dev/ttyACM0 -s 115200
```

### OTA setup

Do this before the first USB flash that should enable OTA updates:

```sh
cp .env.example .env
openssl rand -hex 32
# Paste that value as VIEWE_OTA_TOKEN=... in .env.
python3 tools/create_ota_keys.py
```

Keep `.env` and `secrets/ota_signing_private.pem` on this trusted build
machine. Do not commit either file. The first USB flash embeds the shared token
and public signing key in the meter.

Also re-flash over USB for recovery, or whenever changing the OTA token,
signing key, bootloader, partition table, or LittleFS contents.

### Linux mDNS support

The meter advertises its hostname through its built-in mDNS service, so
`meter1.local` requires mDNS name resolution on the computer running the OTA
tool. On Debian/Ubuntu Linux, install it once with:

```sh
sudo apt install avahi-daemon libnss-mdns
```

This is not required when using `--host <IP-address>`.

### OTA update: one meter

For a meter named `meter1` in Settings → Setup:

```sh
python3 tools/ota.py meter1
```

This builds, signs, uploads to `meter1.local`, and waits for the reboot. If
`.local` discovery is unavailable, use the Station/AP IP shown by the meter:

```sh
python3 tools/ota.py --host 192.168.1.217
```

### OTA update: several meters, one build

Pass all device names in one command. It builds and signs once, then uploads
that same release/version to each meter in order:

```sh
python3 tools/ota.py meter1 meter2 meter3
```

### Reuse the same release later

`tools/ota.py` leaves its signed release in `dist/latest/`. Upload that exact
release to another meter without building again:

```sh
python3 tools/ota_upload.py --device meter4 --release dist/latest
```

Use `--host <IP>` instead of `--device <name>` when connecting through an AP
or when mDNS is unavailable.

For release internals, signature details, and recovery behavior, see
[docs/OTA.md](docs/OTA.md).

## WSL
Expose Windows USB devices to Linux using [usbipd](https://github.com/dorssel/usbipd-win)

## Layout

- `src/main.cpp`: board, display, LVGL, storage, and network startup
- `src/ui/navigation/`: top-level and Settings tab registration
- `src/ui/screens/`: top-level Sensors, Power, and Usage screens
- `src/ui/screens/settings/`: Wi-Fi, Setup, Info, and Debug Settings sub-pages
- `src/ui/components/`, `input/`, and `theme/`: shared UI building blocks
- `src/sensors/`: sensor acquisition, simulated source, and ESP32 ADC source
- `src/sensors/sensor_config.h`: source selection and provisional pin mapping
- `src/data/`: minute-level historical storage
- `docs/HISTORY_STORAGE_V3.md`: agreed segmented history/anchor format and query plan
- `include/esp/`: ESP Display Panel, ESP utility, and LVGL configuration headers

## Remote display and control

Once the meter is on Wi-Fi or its local AP, the authenticated HTTP service
also exposes the actual LCD framebuffer and a virtual touch input:

```sh
# Use the same bearer token configured for signed OTA updates.
curl -H "Authorization: Bearer $VIEWE_OTA_TOKEN" \
  http://device1.local/api/v1/display/screenshot.bmp -o display.bmp

# A raw PPM variant is convenient for CLI image tools.
curl -H "Authorization: Bearer $VIEWE_OTA_TOKEN" \
  'http://device1.local/api/v1/display/screenshot.bmp?format=ppm' -o display.ppm

curl -X POST -H "Authorization: Bearer $VIEWE_OTA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"x":160,"y":240}' \
  http://device1.local/api/v1/display/tap
```

Open `http://device1.local/remote` for **power-meter remote**, a small browser
viewer. It remembers the bearer token in that browser for that device, refreshes
the true 320×480 display about once per second, and maps a click/tap back to
the display.

## Browser time contribution

A local browser application can give the meter a time anchor without internet
access. The endpoint uses the same bearer token as OTA and remote display:

```sh
curl -X POST -H "Authorization: Bearer $VIEWE_OTA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"unix_ms":1783890123456,"utc_offset_minutes":-360}' \
  http://device1.local/api/v1/time/anchor
```

`unix_ms` is UTC milliseconds since the Unix epoch. `utc_offset_minutes` is
local time minus UTC (Denver daylight time is `-360`); it is optional and, when
present, becomes the meter's persisted fixed offset. The future web app should
send `Date.now()` and negate JavaScript's `Date#getTimezoneOffset()` result.

History-file diagnostics are exposed without scanning measurement rows:

```sh
curl -H "Authorization: Bearer $VIEWE_OTA_TOKEN" \
  'http://device1.local/api/v1/history/files?offset=0&limit=25'
```

The endpoint is paginated and caps a page at 50 files.

## Sensor source

Simulation is enabled by default so UI and storage work without sensor
hardware. `src/sensors/sensor_config.h` contains the provisional sequential
mapping: In voltage/current = GPIO 5/6, Out = 7/8, and Aux = 9/10. Set
`POWER_METER_USE_SIMULATED_SENSORS` to `0` when the physical hardware is ready
for validation.

## Firmware constraints and development gotchas

This is a microcontroller firmware, not a database server. New features should
prefer bounded work, small sequential writes, fixed-size data, and information
that can be obtained from filenames/file sizes or current RAM state. In
particular:

- never scan all historical measurement records during boot or normal UI
  refresh;
- keep directory listings bounded (the planned history cap is 200 segment
  files);
- avoid large derived indexes when the filesystem can be the index;
- make expensive per-file inspection an explicit user/API operation;
- preserve sensor sampling and UI responsiveness while filesystem or network
  work is underway.

All top-level and Settings screens are persistent LVGL objects. Recurring screen
timers must check `lv_obj_is_visible()` and do no substantive work for hidden
tabs. Filesystem catalogs should be built once and updated when storage mutates,
not re-enumerated on each UI/API read. Treat an LVGL callback above roughly
50 ms as a performance bug requiring measurement and redesign.

Internal capability RAM is considerably scarcer than the module's 8 MB PSRAM.
CPU-only LVGL metadata/strings and bounded history/API scratch buffers therefore
prefer PSRAM; DMA/display buffers use their explicit allocator. Avoid large
automatic arrays in the Arduino `loopTask` or network handlers. Settings ->
Debug reports internal and PSRAM heap usage separately and includes each heap's
largest free block, which is often more useful than total free bytes when
diagnosing fragmentation.

LVGL's `lv_snprintf()` build does not support floating-point `%f` formats
(including forms such as `%.2f`); it emits the literal `f` instead of a number.
Use standard `snprintf()` where float formatting is required, or convert/round
the value to an integer representation before calling `lv_snprintf()`.
