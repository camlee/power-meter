# ESP32 Power Meter Firmware

PlatformIO Arduino firmware for an offline-first power meter. The same runtime
serves the VIEWE ESP32-S3 touchscreen and the web-first ESP32-WROOM target.
Measurements, local display, and history work without Wi-Fi or wall-clock time.

The product currently provides:

- Solar (`In`), Load (`Out`), and optional Battery (`Aux`) measurements;
- Demo, ESP32 ADC, ADS1115, and versioned UART sensor sources;
- live local UI plus a browser UI with history, setup, calibration, and
  diagnostics;
- minute-level history with explicit gaps and delayed wall-clock anchoring;
- signed local and Internet OTA updates; and
- remote touchscreen viewing/control on touch-enabled builds.

## Targets

| Environment | Hardware | Local surface | Build profile |
| --- | --- | --- | --- |
| `viewe` | VIEWE ESP32-S3, 16 MB flash, OPI PSRAM | LVGL touchscreen + Web | Touch UI and built-in ADC |
| `wroom` | WEMOS LOLIN32 / ESP32-WROOM, 4 MB flash | Web + SSD1306 status display | ADS1115 and no LVGL |

The active VIEWE panel board and feature flags are defined in
`platformio.ini` and `include/esp/`. Hardware-specific sensor wiring is kept in
`src/sensors/sensor_config.h`.

## Build and test

From this directory:

```sh
pio run -e viewe
pio run -e wroom
pio test -e native
```

The web application can be checked independently:

```sh
cd web
npm ci
npm run check
```

The PlatformIO pre-build step runs the web build and embeds its generated
assets in the firmware. History remains in LittleFS; the web assets are part
of the signed application image.

## USB recovery and finding a device

USB flashing remains the recovery path for a new device, an intentional
downgrade, partition/bootloader changes, LittleFS changes, or a device that
cannot join the network:

```sh
pio run -e viewe -t upload --upload-port /dev/ttyACM0
pio run -e wroom -t upload --upload-port /dev/ttyUSB0
```

The firmware prints its current station/AP address and hostname at boot and
when networking changes. Discover a named device with:

```sh
python3 tools/discover_device.py
python3 tools/discover_device.py --host 192.168.1.217
```

Use the displayed IP when mDNS is unavailable or the network isolates
multicast clients.

## OTA

Create the signing key and `.env` once on a trusted build machine, then use
the project OTA tools for signed updates:

```sh
cp .env.example .env
python3 tools/create_ota_keys.py
python3 tools/ota.py meter1
```

Publish a stable Internet release with:

```sh
python3 tools/publish_github_release.py 0.1.2
```

See [docs/OTA.md](docs/OTA.md) for the signing boundary, release workflow,
automatic-update behavior, and recovery rules.

## Project map

- `src/application_runtime.cpp`: common sensing, history, network, web, and OTA lifecycle
- `src/sensors/`: source drivers, calibration, mapping, validation, and derived readings
- `src/data/`: minute history storage, queries, and energy aggregation
- `src/time/`: sessions, wall-clock anchors, and timeline reconciliation
- `src/network/`: Wi-Fi, HTTP/WebSocket APIs, OTA, and remote display
- `src/ui/`: LVGL screens and shared UI services
- `web/`: Svelte application and browser-side binary protocol parsers
- `tools/`: build, release, discovery, OTA, and demo-profile helpers

## Documentation

- [Architecture](docs/ARCHITECTURE.md): service boundaries and durable invariants
- [Sensor data policy](docs/SENSOR_DATA_POLICY.md): states, eligibility, and coverage semantics
- [History and time](docs/HISTORY.md): storage, datasets, recovery, and time anchoring
- [UART sensor](docs/UART_SENSOR.md): wiring and the external producer protocol
- [OTA](docs/OTA.md): signed update and recovery procedures
- [Web app](docs/WEB_APP.md): embedded frontend, API boundaries, and synchronization rules

The code is authoritative for implementation details and exact binary layouts.
These documents preserve the decisions that are otherwise easy to miss.
