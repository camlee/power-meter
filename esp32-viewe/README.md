# ESP32 Power Meter Firmware

PlatformIO Arduino project with two hardware profiles sharing one runtime and
embedded web application.

## Hardware targets

| Environment | OTA identity | Hardware | Local UI | Flash/RAM |
| --- | --- | --- | --- | --- |
| `viewe` | `meter-viewe` | VIEWE ESP32-S3 | LVGL touch display | 16 MB flash, OPI PSRAM |
| `wroom` | `meter-wroom` | WEMOS LOLIN32 / ESP32-WROOM | Web + SSD1306 status | 4 MB flash, internal RAM |

The active ESP Display Panel board is configured in `include/esp/esp_panel_board_supported_conf.h`:

```cpp
#define BOARD_VIEWE_UEDX32480035E_WB_A
```

## Building and testing

Build the firmware with PlatformIO:

```sh
pio run -e viewe
pio run -e wroom
```

Run the host-side PM1 UART parser tests locally, without an attached ESP32:

```sh
pio test -e native
```

## Updating firmware

Development builds derive a short SemVer-compatible identity from the nearest
`v*` tag using `git describe`, such as `0.1.0-3+g1a2b3c4`. Stable releases take
one `MAJOR.MINOR.PATCH` version from their Git tag so both hardware artifacts
share the exact release identity.

### Publish an Internet OTA release

Commit the release source, then run one command with the new version:

```sh
python3 tools/publish_github_release.py 0.1.2
```

It tests the project, builds and signs both hardware targets, creates and
pushes `v0.1.2` with the current branch, and uploads all eight assets to a
draft GitHub release. Open the URL it prints, review the draft, and publish it.
The OTA private key remains only on the local build machine.

### USB / hardwired update

Connect one meter over USB, then run:

```sh
pio run -e viewe -t upload --upload-port /dev/ttyACM0
pio run -e wroom -t upload --upload-port /dev/ttyUSB0
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
python3 tools/create_ota_keys.py
```

Keep `.env` and `secrets/ota_signing_private.pem` on this trusted build
machine. Do not commit either file. The first USB flash embeds the public
signing key in the meter.

Also re-flash over USB for recovery, or whenever changing the signing key,
bootloader, partition table, or LittleFS contents. See
[`docs/OTA.md`](docs/OTA.md) for local and GitHub release workflows.

### Linux mDNS support

The meter advertises its hostname and both `_http._tcp` and
`_viewe-ota._tcp` services through its built-in mDNS service, so
`tools/ota.py`, `tools/ota_upload.py`, and `tools/discover_device.py` include a
bounded, dependency-free Python mDNS fallback. Check one or more names directly
with:

```sh
python3 tools/mdns_resolver.py meter1 sensor1
```

Avahi remains useful when `.local` names should also work in `curl`, browsers,
and other operating-system clients. On Debian/Ubuntu Linux, install it once
with:

```sh
sudo apt install avahi-daemon libnss-mdns
```

Neither mDNS method can cross a multicast-isolated network. In that case, use
`--host <IP-address>` with the address shown on the display or serial console.
The numeric Station IP shown on the meter is also the reliable browser fallback
for phones whose OS/browser, VPN, or Wi-Fi network does not resolve `.local`
names. Both forms open the same web application:

```text
http://meter1.local/
http://192.168.1.217/
```

### OTA update: one meter

For a meter named `meter1` in Settings → Setup:

```sh
python3 tools/ota.py -e wroom meter1
```

This builds, signs, resolves `meter1.local` to a stable IP for the upload and
reboot check, and waits for firmware confirmation. If multicast discovery is
unavailable, use the Station/AP IP shown by the meter:

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
- `src/ui/screens/`: top-level Home, Usage, Power, and Sensors screens
- `src/ui/screens/settings/`: Wi-Fi, Info, Setup, Data, and Debug Settings sub-pages
- `src/ui/components/`, `input/`, and `theme/`: shared UI building blocks
- `src/sensors/`: sensor acquisition plus Demo, ESP32 ADC, ADS1115, and UART sources
- `src/sensors/sensor_config.h`: source selection and provisional pin mapping
- `src/data/`: minute-level historical storage
- `docs/HISTORY_STORAGE_V1.md`: candidate segmented history/tenant/coverage contract
- `docs/HISTORY_TEST_PLAN.md`: deterministic, accelerated, interruption, and soak verification
- `docs/SENSOR_DATA_POLICY.md`: raw observation, calculation limits, null/stale, and coverage policy
- `docs/UART_SENSOR.md`: versioned UART sensor protocol and current Uno wiring
- `tools/build_demo_profile.py`: deterministic `demo-source` fixture generator/check
- `include/esp/`: ESP Display Panel, ESP utility, and LVGL configuration headers

## Remote display and control

On the `viewe` target, the local web interface exposes
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

The normal firmware image contains a small Svelte web application at
`http://device1.local/`. It displays live readings, contributes browser time,
and provides station/AP Wi-Fi management. Touchscreen targets also include the
remote-display view. These trusted-LAN controls do not require a browser token. Its frontend
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
curl 'http://device1.local/api/v1/history/files?offset=0&limit=25'
```

The endpoint is paginated and caps a page at 50 files.

## Sensor source

Simulation is enabled by default so UI and storage work without sensor
hardware. `src/sensors/sensor_config.h` contains the physical VIEWE wiring:
Sensor 1 voltage/current GPIO 6/5, Sensor 2 GPIO 10/9, and Sensor 3 GPIO 8/7.
A versioned per-source mapping then assigns those physical sensors to logical
Solar, Load, Battery, or Unmapped roles and selects current direction. Defaults
are Sensor 1 = Solar, Sensor 2 = Load, and Sensor 3 = Battery. The ESP32 ADC
Sensor 3 direction is reversed after calibration so charging is positive
without changing its persisted gain/offset or raw input voltage; Demo, UART,
and ADS1115 retain normal direction defaults. Battery may be unmapped, in which
case power is inferred from Solar minus Load and Load voltage is used as the
Battery-voltage fallback. Set
`POWER_METER_USE_SIMULATED_SENSORS` to `0` when the physical hardware is ready
for validation.

`GET /api/v1/sensors/mapping` exposes physical readings and the active mapping.
`PUT /api/v1/sensors/mapping` persists a complete active-source profile and
restarts the meter. On the touchscreen, Setup's edit button opens the active
source's full-screen mapping editor with live physical V/A/W readings and
draft-aware Balance. Web Setup provides the same editor as a responsive inline
panel: beside Setup on desktop and stacked below the sensor summary on phones.
It loads the persisted profile once, then receives physical diagnostics from
the shared live WebSocket rather than polling HTTP. Balance is hidden from
Usage and Power by default and can be enabled in either mapping editor. The
V/A edit icons open one shared full-screen calibration workflow for the exact
physical sensor and measurement; its subtitle includes both the mapped role
and `Voltage` or `Current`.

The WROOM build keeps Demo as the fresh-device default but exposes ADS1115 in
Setup. Its SSD1306 (address `0x3c`) and ADS1115 (`0x48`) share GPIO 5 SDA / GPIO
4 SCL. The OLED reports hostname/network/IP and high-level sensor state, with
large Summary and diagnostic Dense layouts. ADS1115 calibration is independent
from ESP32-ADC calibration.

Hardware capabilities are independent build flags:

```text
POWER_METER_HAS_STATUS_DISPLAY
POWER_METER_HAS_ESP32_ADC
POWER_METER_HAS_ADS1115
```

The normal VIEWE target disables ADS1115, but enabling it uses the ESP-IDF I2C
backend on a separately configured port/pin pair rather than Arduino Wire,
which cannot coexist with the VIEWE panel library's legacy I2C driver.

Physical ADC sources acquire calibrated voltage/current pairs continuously and
reduce those observations into the normal 500 ms reading stream. The ESP32 ADC
targets 5 ms; the four-conversion WROOM ADS1115 path targets 15 ms. These are
scheduler targets, and every diagnostic capture reports its measured interval.
An isolated rejected observation no longer invalidates its entire reducer
window: at least 80% valid coverage produces a valid mean from valid samples
only. Sustained out-of-range input remains visible for diagnosis.

Raw observations are retained only for an on-demand capture. In the browser,
select **View Raw** beside a configured channel on Sensors. On the VIEWE
display, use the image icon at the right of that channel's KPI row. A capture
contains voltage, current, and instantaneous power for one selected logical
channel across three consecutive 500 ms production windows, alongside the
actual reduced voltage/current/power/duty values. It uses the same calibrated
observations as normal energy accounting and is discarded after the UI takes
the result. Capture IDs protect the shared browser/display service from stale
take or cancel operations; abandoned results expire automatically.

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
diagnosing fragmentation. On the touchscreen target it also reports the LVGL
task's configured stack size and minimum free bytes observed since boot.

LVGL's `lv_snprintf()` build does not support floating-point `%f` formats
(including forms such as `%.2f`); it emits the literal `f` instead of a number.
Use standard `snprintf()` where float formatting is required, or convert/round
the value to an integer representation before calling `lv_snprintf()`.
