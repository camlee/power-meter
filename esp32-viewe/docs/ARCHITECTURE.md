# Viewe architecture

## Purpose

`esp32-viewe` is the active power-meter firmware. It runs on a VIEWE
ESP32-S3 touch-display module and is an **offline-first solar power meter**:
live, accurate on-device measurements are the primary product; Wi-Fi adds
configuration, synchronization, remote views, and future peer features.

The current firmware is a meter/display. Its service boundaries should permit
two roles in a future multi-device phase:

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
| `Aux` | Reserved independent measurement | Excluded from net battery power |

The final local ADC target has a voltage and current input for each channel, for
six inputs on provisional GPIO 5 through 10. Sources and devices may provide
only a subset of the logical channels. Presence is explicit configuration/data;
a floating ADC value is not attachment detection. The exact final pin/polarity
map is part of the future on-site hardware-integration milestone.

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
Demo / ADC / Uno UART sources
        |
        v
Sensor source contract -------> sensors (validation, buffers, derived values)
        |                                |
        |                                +--> live UI
        v
Measurement/energy aggregation --> persistent history --> historical UI/API
        |
        +--> clock/timeline reconciliation

Network services --> Wi-Fi UI, NTP/browser time, web app/API, OTA, remote display
```

### 1. Sensor acquisition

`sensors/` owns acquisition and exposes thread-safe normalized readings.
`SensorSource` is the acquisition boundary. A source returns channel presence,
health, timestamps, voltage/current, and optionally direct duty. ADC values are
raw inputs to ESP32 calibration; Demo and UART values are already engineering
units. The service validates values, applies calibration exactly once, derives
power once, and owns short in-memory histories.

Realtime Demo remains valuable for exercising the complete live/history path.
The ESP32 ADC source is the final local target. The interim Arduino Uno UART
source provides calibrated `In`/`Out` readings and duty while `Aux` is absent.
One UART frame supplies a coherent multi-channel snapshot, so its receiver is a
shared frame source rather than three independent serial readers.

Requirements for the production source:

- fixed channel configuration and a clear channel/pin table;
- calibrated millivolt reads, suitable attenuation, and bounded filtering;
- per-channel persisted offset/gain values with safe defaults;
- independent channel presence and explicit waiting/invalid/stale states;
- finite/plausibility validation for every source, including the local ADC,
  while retaining finite raw observations on Sensors for diagnosis;
- no integration across an unavailable interval and no indefinite reuse of a
  stale last-good value;
- no blocking work while holding the shared sensor-history mutex.

`In - Out` is net battery power only when both inputs are valid: positive
charges the battery and negative discharges it. Derived metrics that lack their
required channels are unavailable, not zero. Values are never clamped into the
calculation range because that would fabricate power/energy. `Aux` remains
independent. See `SENSOR_DATA_POLICY.md` for states, limits, coverage, and
calibration implications.

### 2. UI

LVGL owns the display and touch UI. `ScreenManager` provides the persistent
top-level tab layout; screens should only consume public service APIs, never
read hardware or files directly.

Current top-level screens are:

- **Sensors:** raw per-sensor voltage/current/power and short trend charts.
- **Power:** derived battery and solar/PWM-related metrics.
- **Usage:** rolling and calendar history with gaps/time uncertainty.
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

Energy is integrated from eligible sample power over elapsed monotonic time.
The disposable alpha format called V3 is being replaced without migration by
candidate History V1. Each fixed 56-byte minute row stores channel/component
energy, valid coverage, configured-channel mask, and quality flags. Dataset,
session, and minute identity live in segment paths/names.

The persistent format must distinguish two concepts:

- **Measurement order / monotonic time:** always available within a boot
  session and across sessions via an incrementing record/session sequence.
- **Wall-clock time:** optional metadata that can be attached or revised after
  NTP, a browser, or a peer supplies a trustworthy time anchor.

This permits continuous ordered history when wall time is absent, distinguishes
measured zero from partial/missing coverage, and later maps unsynchronized
records into an honest time window without inventing a precise timestamp.

Real and Demo are logical tenants of the same engine. ADC/UART route to Real;
realtime simulation routes to Demo. The Settings -> History segmented control
is a view filter only. Protected Demo fixture files use reserved session zero
and fixed past time; they coexist with ordinary recorded Demo sessions without
sliding or overlap. Seeding/versioning never touches Real.

Five rows are buffered in PSRAM and appended as one 280-byte write. Segments
close after 240 records (about four hours). Stale `.open` files retain complete
rows and ignore a torn tail. Independent 200-file caps prevent Demo activity
from evicting Real history. The bounded anchor ledger remains device-wide and
keys normal entries by globally unique boot session; catalogs and queries
always select one measurement dataset and seek directly to overlapping rows.
See `HISTORY_STORAGE_V1.md` for the candidate format,
fixture, recovery, inference, retention, and reset rules.

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

The implemented local service layer includes:

- a realtime/history browser API and embedded web application;
- browser time contribution;
- signed OTA delivery and diagnostics;
- remote display capture and input on a trusted local network.

Browser calibration/settings/storage pages remain incremental product backlog.
Data export and synchronization between devices remain future work.

Mesh networking is a future architectural option, not a near-term dependency;
the first network contract should work over ordinary Wi-Fi AP/station mode.

## Configuration and persistence

Configuration belongs in NVS/Preferences and is versioned. It includes sensor
source mode, channel presence/mapping, calibration values, device identity, and
Wi-Fi/AP settings. The current persisted Demo/Real boolean must migrate to a
versioned mode enum before UART is added while preserving existing settings.
Future peer/service settings are out of the current phase.

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

## Active implementation gaps

These are documented design gaps, not accepted final behavior:

- Sensor readings do not yet carry production-ready channel presence,
  validity/staleness, provenance, or error diagnostics through every consumer.
- Candidate History V1 tenant isolation, per-channel coverage, protected Demo
  fixtures, and dataset-scoped reset/retention are designed but not implemented.
- History/time needs deterministic recovery/query tests, accelerated retention
  tests, and real rotation/multi-day verification before it is production-durable.
- The Arduino Uno UART source and its versioned protocol are specified but not
  implemented.
- Physical ADC conversion/calibration accuracy remains a separate future
  on-site integration milestone.
- Role configuration and peer synchronization are explicitly out of scope.

## Development and verification

PlatformIO is the canonical local build path: `pio run -d esp32-viewe`.
The currently checked configuration builds successfully with a 3 MB OTA slot.

Development keeps realtime simulation selectable and routes it through the same
validation/history path as physical sources. Source health must be observable
through local and web diagnostics. Raw ADC millivolts plus converted readings
remain available for the future hardware-integration procedure. See
`SCOPE_AND_ROADMAP.md`, `SENSOR_DATA_POLICY.md`, `HISTORY_STORAGE_V1.md`,
`ARDUINO_UART_SENSOR.md`, and `HISTORY_TEST_PLAN.md` for the active delivery
sequence and acceptance work.
