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
- `src/ui/`: screen navigation and UI screens; the top-level Settings screen
  contains Wi-Fi, Setup, Info, and Debug sub-pages
- `src/sensors/`: sensor acquisition, simulated source, and ESP32 ADC source
- `src/sensors/sensor_config.h`: source selection and provisional pin mapping
- `src/data/`: minute-level historical storage
- `include/esp/`: ESP Display Panel, ESP utility, and LVGL configuration headers

## Sensor source

Simulation is enabled by default so UI and storage work without sensor
hardware. `src/sensors/sensor_config.h` contains the provisional sequential
mapping: In voltage/current = GPIO 5/6, Out = 7/8, and Aux = 9/10. Set
`POWER_METER_USE_SIMULATED_SENSORS` to `0` when the physical hardware is ready
for validation.
