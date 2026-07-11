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

Current screens are:

- **Now:** raw per-sensor voltage/current/power and short trend charts.
- **Power:** derived battery and solar/PWM-related metrics.
- **Usage:** long-term history (currently provisional).
- **Wi-Fi:** station scan/connect plus local AP control.
- **Info:** device/build, memory, storage, clock, and reset information.

The display is a first-class offline interface. Network operations must be
asynchronous and must not stall sampling or rendering.

### 3. Energy and durable history

Energy is integrated from sample power over elapsed monotonic time. The system
stores minute buckets containing average power and energy per channel. It must
also maintain durable cumulative counters, checkpointed frequently enough that
a power interruption loses only a bounded, documented amount of energy.

The persistent format must distinguish two concepts:

- **Measurement order / monotonic time:** always available within a boot
  session and across sessions via an incrementing record/session sequence.
- **Wall-clock time:** optional metadata that can be attached or revised after
  NTP, a browser, or a peer supplies a trustworthy time anchor.

This permits continuous ordered history when wall time is absent, represents
outages as gaps, and later maps unsynchronized records into an honest time
window without inventing a precise timestamp.

The history storage API should therefore remain independent of the clock and
support buckets by minute/hour/day. Flash writes should be batched and made
crash-tolerant (versioned record/header, checksums or commit markers, recovery
of an incomplete final batch). Retention must be calculated from actual file
rotation behavior and available space, then exposed in Info.

### 4. Time service

Introduce a dedicated time service rather than placing time logic in Wi-Fi or
history code. It maintains an estimate and provenance for wall time:

1. boot with no wall time, but a fresh monotonic session;
2. accept a time anchor from NTP when available;
3. accept explicitly supplied browser time, with source metadata;
4. later accept authenticated/specified peer time;
5. persist anchors and use them to estimate intervals for prior/later records.

NTP is preferred when reachable, but is not required for measuring or storing
energy. Browser and peer time are redundant sources, not replacements for the
offline timeline.

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
- Flash operations are buffered and scheduled so they cannot starve sampling.
- Every persisted format is versioned and recoverable after abrupt power loss.
- A sensor failure, clock loss, Wi-Fi loss, or peer loss degrades a feature but
  never prevents local measurement/UI operation.

## Current implementation gaps

These are documented design gaps, not accepted final behavior:

- simulation is the default until physical hardware is available; the ADC
  source and provisional pin/channel configuration are available but untested;
- no persisted calibration exists;
- historical records write `epoch_s = 0` and are not tied to a time service;
- the current two-file history rotation does not meet its stated 30-day goal;
- the Usage chart has provisional channel/math semantics;
- Wi-Fi startup ownership is unclear: `main.cpp` does not explicitly initialize
  or service the network manager;

## Development and verification

PlatformIO is the canonical local build path: `pio run -d esp32-viewe`.
The currently checked configuration builds successfully with a 3 MB OTA slot.

Development should keep simulation as a selectable mode, add serial/API
diagnostics for every service, and add a hardware test screen for raw ADC
millivolts plus converted readings. Future OTA and remote verification should
be layered only after the local build, flash, serial log, and recovery paths
are dependable.
