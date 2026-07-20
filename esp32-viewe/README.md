# ESP32 Power Meter Firmware

PlatformIO Arduino project with two hardware profiles sharing one runtime and
embedded web application.

## Hardware targets

| Environment | OTA identity | Hardware | Local UI | Flash/RAM |
| --- | --- | --- | --- | --- |
| `meter` | `meter-viewe` | VIEWE ESP32-S3 | LVGL touch display | 16 MB flash, OPI PSRAM |
| `wroom32` | `meter-wroom` | WEMOS LOLIN32 / ESP32-WROOM | Web only initially | 4 MB flash, internal RAM |

The active ESP Display Panel board is configured in `include/esp/esp_panel_board_supported_conf.h`:

```cpp
#define BOARD_VIEWE_UEDX32480035E_WB_A
```

## Building and testing

Build the firmware with PlatformIO:

```sh
pio run -e meter
pio run -e wroom32
```

Run the host-side PM1 UART parser tests locally, without an attached ESP32:

```sh
pio test -e native
```

## Updating firmware

Firmware versions are generated as
`0.0.N` and increment once per build. A single build can therefore update many
meters with the exact same signed firmware.

### USB / hardwired update

Connect one meter over USB, then run:

```sh
pio run -e meter -t upload --upload-port /dev/ttyACM0
pio run -e wroom32 -t upload --upload-port /dev/ttyUSB0
```

To watch WROOM logs:

```sh
screen /dev/ttyUSB0 115200
```

At boot and whenever networking changes, the meter prints a machine-readable
address line such as `VIEWE_WEB url=http://192.168.1.217/ host=meter1.local`.
That is the quickest physical-device recovery path. On a machine with mDNS,
the project helper can discover the advertised meter without a subnet scan:

```sh
python3 tools/discover_device.py
# Or use an address copied from serial:
python3 tools/discover_device.py --host 192.168.1.217
```

A device with no saved Wi-Fi or AP configuration starts an access point named
`meterXX`, using the final two characters of its generated device identity.
Set `POWER_METER_AP_SSID` and optionally `POWER_METER_AP_PASSWORD` in the
ignored `.env` to override it. Without a password the initial AP is open.

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
python3 tools/ota.py -e wroom32 meter2
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

- `src/main.cpp`: shared Arduino entry point plus optional touch-UI startup
- `src/application_runtime.cpp`: common sensing, storage, network, web, and OTA lifecycle
- `src/device/hardware_profile.h`: compile-time hardware capabilities
- `src/ui/navigation/`: top-level and Settings tab registration
- `src/ui/screens/`: top-level Sensors, Power, and Usage screens
- `src/ui/screens/settings/`: Wi-Fi, Setup, Info, and Debug Settings sub-pages
- `src/ui/components/`, `input/`, and `theme/`: shared UI building blocks
- `src/sensors/`: sensor acquisition plus Demo, ESP32 ADC, and UART sources
- `src/sensors/sensor_config.h`: source selection and provisional pin mapping
- `src/data/`: minute-level historical storage
- `docs/HISTORY_STORAGE_V1.md`: candidate segmented history/tenant/coverage contract
- `docs/HISTORY_TEST_PLAN.md`: deterministic, accelerated, interruption, and soak verification
- `docs/SENSOR_DATA_POLICY.md`: raw observation, calculation limits, null/stale, and coverage policy
- `docs/UART_SENSOR.md`: versioned UART sensor protocol and current Uno wiring
- `tools/build_demo_profile.py`: deterministic `demo-source` fixture generator/check
- `include/esp/`: ESP Display Panel, ESP utility, and LVGL configuration headers

## Remote display and control

On the `meter-viewe` target, the local web interface exposes
the actual LCD framebuffer and a virtual touch input without a browser token:

```sh
curl http://device1.local/api/v1/display/screenshot.bmp -o display.bmp

# A raw PPM variant is convenient for CLI image tools.
curl 'http://device1.local/api/v1/display/screenshot.bmp?format=ppm' -o display.ppm

curl -X POST -H 'Content-Type: application/json' \
  -d '{"x":160,"y":240}' \
  http://device1.local/api/v1/display/tap
```

Open `http://device1.local/remote` for the embedded remote page. It loads a
full-resolution snapshot immediately and maps clicks, drags, and taps back to
the display. These controls are intended only for a trusted local network.

## Embedded web application

The normal firmware image now contains a small Svelte web application at
`http://device1.local/`. It displays live readings, contributes browser time,
and includes the remote-display view without requiring a browser token. Its frontend
assets ship with normal USB and signed OTA application updates; they do not use
the history LittleFS partition. See [docs/WEB_APP.md](docs/WEB_APP.md) for the
local Node/PlatformIO build workflow, caching policy, realtime protocol, and
cross-surface settings synchronization model.

## Browser time contribution

A local browser application can give the meter a time anchor without internet
access. The endpoint is available to the trusted local network:

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"unix_ms":1783890123456,"utc_offset_minutes":-360}' \
  http://device1.local/api/v1/time/anchor
```

`unix_ms` is UTC milliseconds since the Unix epoch. `utc_offset_minutes` is
local time minus UTC (Denver daylight time is `-360`); it is optional and, when
present, becomes the meter's persisted fixed offset. The embedded web app sends
`Date.now()` and negates JavaScript's `Date#getTimezoneOffset()` result.

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

The initial WROOM target intentionally remains in Demo mode. Its existing
ADS1115 and SSD1306 hardware are follow-up integrations, not part of the first
web-UI bring-up.

## Firmware constraints and development gotchas

This is a microcontroller firmware, not a database server. New features should
prefer bounded work, small sequential writes, fixed-size data, and information
that can be obtained from filenames/file sizes or current RAM state. In
particular:

- never scan all historical measurement records during boot or normal UI
  refresh;
- keep directory listings bounded (the history cap is 200 segment
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
