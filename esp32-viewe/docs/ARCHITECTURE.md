# Viewe architecture

## Purpose

`esp32-viewe` is the active power-meter firmware. It runs on a VIEWE
ESP32-S3 touch-display module and is an **offline-first solar power meter**:
live, accurate on-device measurements are the primary product; Wi-Fi adds
configuration, synchronization, remote views, and future peer features.

The same firmware should support two roles selected by configuration:

- **Meter** — reads sensors, persists measurements, and presents the local UI.
- **Peer/display** — has no attached sensors; it can keep/display time and,
  later, exchange time and measurements with a meter.

All functionality must remain useful with no network, NTP, or peer available.

## Hardware and electrical model

There are three independent channels:

| Logical channel | Meaning now | Role in system calculations |
| --- | --- | --- |
| `In` | Solar-panel input to the battery system | Production / charging input |
| `Out` | Battery-system load output | Consumption / discharge output |
| `Aux` | Reserved independent measurement | Excluded |

Each channel has a voltage and current input, for six ESP32 ADC inputs total:
GPIO 5, 6, 7, 8, 9, and 10. The exact GPIO-to-channel/polarity mapping must be
defined in one central configuration before real hardware is enabled.

Initial nominal transfer functions, applied to calibrated ADC input voltage in
volts, are:

```text
measured_voltage_V = (adc_V - 0.000) / 0.027027
measured_current_A = (adc_V - 1.667) / 0.026667
power_W            = measured_voltage_V * measured_current_A
```

The ADC input range is expected to be 0–3.3 V. This is an electrical design
constraint, not a promise that every ESP32 ADC reading is intrinsically
accurate; the driver must use the Arduino/ESP-IDF calibrated millivolt API and
hardware validation will establish per-channel offsets and gains.

## Layering

```text
Physical ADC inputs
        |
        v
SensorSource implementations --> sensors (sampling, buffers, derived values)
        |                                |
        |                                +--> live UI
        v
Measurement/energy aggregation --> persistent history --> historical UI/API
        |
        +--> clock/timeline reconciliation

Network manager --> Wi-Fi UI, NTP, browser/peer time sources, future API/OTA
```

### 1. Sensor acquisition

`sensors/` owns sampling and exposes thread-safe readings. `SensorSource` is
the hardware boundary: its implementations only return raw voltage/current;
the `sensors` service derives power once and owns short in-memory histories.

The current simulated source remains valuable for UI and storage development.
The production ESP32 ADC source is selected from the central channel
configuration when simulation is disabled.

Requirements for the production source:

- fixed channel configuration and a clear channel/pin table;
- calibrated millivolt reads, suitable attenuation, and bounded filtering;
- per-channel persisted offset/gain values with safe defaults;
- explicit handling of unavailable/invalid readings, rather than treating a
  failing sensor as valid zero power;
- no blocking work while holding the shared sensor-history mutex.

`In - Out` is net battery power: positive charges the battery, negative
discharges it. `Aux` remains independent until a future configuration model
gives it a defined system role.

### 2. UI

LVGL owns the display and touch UI. `ScreenManager` provides the persistent
top-level tab layout; screens should only consume public service APIs, never
read hardware or files directly.

Current top-level screens are:

- **Sensors:** raw per-sensor voltage/current/power and short trend charts.
- **Power:** derived battery and solar/PWM-related metrics.
- **Usage:** long-term history (currently provisional).
- **Settings:** nested configuration and diagnostics pages:
  - **Wi-Fi:** station scan/connect, station IP/RSSI, plus local AP control
    and its gateway IP;
  - **Setup:** persisted device ID/hostname (`meter-...`) and hardware ID;
  - **Info:** build, current date/time, uptime, and current station/AP IPs;
  - **History:** bounded, filename-driven segment diagnostics and live RAM state;
  - **Debug:** SDK/chip/reset details, disjoint internal/PSRAM heap usage and
    largest free blocks, storage, and OTA diagnostics.

The display is a first-class offline interface. Network operations must be
asynchronous and must not stall sampling or rendering.

### 3. Energy and durable history

Energy is integrated from sample power over elapsed monotonic time. History V3
stores one fixed 32-byte energy row per complete monotonic minute. Session and
minute identity live in the segment filename, avoiding repeated metadata in
every row.

The persistent format must distinguish two concepts:

- **Measurement order / monotonic time:** always available within a boot
  session and across sessions via an incrementing record/session sequence.
- **Wall-clock time:** optional metadata that can be attached or revised after
  NTP, a browser, or a peer supplies a trustworthy time anchor.

This permits continuous ordered history when wall time is absent, represents
outages as gaps, and later maps unsynchronized records into an honest time
window without inventing a precise timestamp.

The clock-independent API supports rolling and calendar buckets. Five rows are
buffered in PSRAM and appended as one 160-byte write. Each boot begins a new
`.open` session segment; a segment closes to `.bin` after 240 records (about
four hours). Stale `.open` files remain readable after an abrupt reboot, with
only incomplete trailing bytes ignored. A 200-measurement-file cap bounds
directory and retention work; under normal six-file/day operation it holds
about 33 days, while frequent reboots intentionally shorten that window.

The small `/history/v3/time-anchors.bin` ledger retains at most one useful
anchor per session and is loaded as a bounded in-memory table. The filename
catalog is built once at boot and maintained incrementally as storage changes.
Calendar queries resolve only candidate intervals and seek directly to
overlapping 32-byte rows. See `HISTORY_STORAGE_V3.md` for the exact
format, recovery, inference, and query rules.

### 4. Time service

A dedicated time service, separate from Wi-Fi and history, maintains wall-time
estimates and provenance:

1. boot with no wall time, but a fresh monotonic session;
2. accept a time anchor from NTP when available;
3. accept explicitly supplied browser time, with source metadata;
4. later accept authenticated/specified peer time;
5. persist anchors and use them to estimate intervals for prior/later records.

NTP is preferred when reachable, but is not required for measuring or storing
energy. Browser and peer time are redundant sources, not replacements for the
offline timeline.

The initial calendar implementation uses a persisted fixed UTC offset. This is
enough for the meter's short (up to two week), predominantly seasonal views and
avoids pretending that a browser's current offset contains complete timezone
rules. Full IANA timezone and DST-transition support can be added later without
changing the UTC anchors or raw measurement records.

An unanchored boot block may be reconciled when anchors bound both sides. Its
total unexplained downtime is explicit timestamp uncertainty and the block is
shown only where that uncertainty fits the selected Usage bucket, retaining
otherwise valid energy without placing it falsely at now.
Missing or uncertain coverage is considered user-significant only when it exceeds
one minute; smaller restart gaps remain in the data model but do not raise the UI
warning symbol.

### 5. Network and future services

`network_manager` owns Wi-Fi state, persisted station credentials, AP mode,
and NTP reachability. It must be initialized from startup and ticked by a
well-defined non-UI owner; UI screens issue commands and display state.

Future capabilities build on an authenticated local service layer:

- browser API and realtime web app;
- data export/sync between devices;
- meter/peer discovery and time sharing;
- OTA update delivery and device diagnostics;
- optional collaborative features such as a shared drawing surface.

Mesh networking is a future architectural option, not a near-term dependency;
the first network contract should work over ordinary Wi-Fi AP/station mode.

## Configuration and persistence

Configuration belongs in NVS/Preferences and is versioned. It includes device
role, sensor channel mapping, calibration values, Wi-Fi/AP settings, and future
peer/service settings. Sensor calibration must be editable from the device UI
and ultimately importable/exportable through the web interface.

Secrets need an explicit security policy before remote access is introduced.
The current plain NVS credential storage is acceptable only as a local-device
starting point; it is not a sufficient design for exposed remote services.

## Concurrency and reliability rules

- Sensor sampling runs independently from LVGL and uses bounded critical
  sections for shared buffers.
- UI updates use LVGL's locking/port conventions; background tasks do not
  manipulate LVGL objects directly.
- Persistent screens make recurring timers visibility-aware. Hidden tabs do not
  query storage, rebuild widgets, or invalidate charts.
- LVGL callbacks remain bounded and non-blocking. Filesystem metadata is cached
  or prepared outside recurring UI refreshes so storage/network latency cannot
  determine touch-processing latency.
- Flash operations are buffered and scheduled so they cannot starve sampling.
- Persisted formats have explicit format identity and bounded recovery behavior.
- A sensor failure, clock loss, Wi-Fi loss, or peer loss degrades a feature but
  never prevents local measurement/UI operation.

## Remaining implementation gaps

These are documented design gaps, not accepted final behavior:

- simulation is the default until physical hardware is available; the ADC
  source and provisional pin/channel configuration are available but untested;
- physical ADC conversion and calibration still require validation against the
  installed electrical hardware;
- session-aware history/time needs longer-running recovery and retention testing
  before its data is treated as production-durable;
- the full meter data API and browser application are not yet implemented;
- role configuration and peer time/data synchronization remain future work.

## Development and verification

PlatformIO is the canonical local build path: `pio run -d esp32-viewe`.
The currently checked configuration builds successfully with a 3 MB OTA slot.

Development should keep simulation as a selectable mode, add serial/API
diagnostics for every service, and add a hardware test screen for raw ADC
millivolts plus converted readings. Future OTA and remote verification should
be layered only after the local build, flash, serial log, and recovery paths
are dependable.
