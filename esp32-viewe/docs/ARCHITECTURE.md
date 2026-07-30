# Viewe architecture

## Purpose

`esp32-viewe` is the active power-meter firmware. It runs on both a VIEWE
ESP32-S3 touch-display module and an ESP32-WROOM web-first module, and is an
**offline-first solar power meter**: live, accurate measurements are the
primary product; Wi-Fi adds configuration, synchronization, remote views, and
future peer features.

Both targets use one application entry point and common runtime. Compile-time
hardware profiles select PSRAM policy, touch/LVGL support, status-display
support, individual sensor-source capabilities, and bounded sensor-history depth. Runtime APIs
publish those capabilities so one embedded web application can adapt without
separate frontend builds.

The current firmware is a meter/display. Its service boundaries should permit
two roles in a future multi-device phase:

- **Meter** — reads sensors, persists measurements, and presents the local UI.
- **Peer/display** — has no attached sensors; it can keep/display time and,
  later, exchange time and measurements with a meter.

All functionality must remain useful with no network, NTP, or peer available.

## Hardware and electrical model

There are three physical measurement channels and three logical roles:

| Logical role | Stable wire/storage ID | Role in system calculations |
| --- | --- | --- |
| **Solar** | `In` | Positive production |
| **Load** | `Out` | Positive consumption |
| **Battery** | `Aux` | Positive charge, negative discharge |

The VIEWE local ADC target has a voltage and current input for each channel:
Sensor 1 uses voltage GPIO 6 and current GPIO 5; Sensor 2 uses voltage GPIO 10
and current GPIO 9; Sensor 3 uses voltage GPIO 8 and current GPIO 7. Each
acquisition source has a versioned NVS profile assigning every physical sensor
to Solar, Load, Battery, or Unmapped and selecting normal/reversed current.
Solar and Load must each be assigned exactly once; Battery is optional.
Presence is explicit configuration/data—a floating ADC value is not attachment
detection.

Initial nominal transfer functions, applied to calibrated ADC input voltage in
volts, are:

```text
measured_voltage_V = (adc_V - 0.000) / 0.027027
measured_current_A = (adc_V - 1.667) / 0.026667
effective_current_A = measured_current_A * current_direction
power_W             = measured_voltage_V * effective_current_A
```

Factory mapping is Sensor 1 = Solar, Sensor 2 = Load, and Sensor 3 = Battery.
The ESP32 ADC profile defaults Sensor 3 to `current_direction = -1`, preserving
the installed meter correction; its other sensors use `+1`. Demo, UART, and
ADS1115 default all directions to `+1`. Direction is applied after physical
calibration, so saved gain/offset values and raw input voltages are unchanged
when a sensor is remapped.

The ADC input range is expected to be 0–3.3 V. This is an electrical design
constraint, not a promise that every ESP32 ADC reading is intrinsically
accurate; the driver must use the Arduino/ESP-IDF calibrated millivolt API and
hardware validation will establish per-channel offsets and gains.

## Layering

```text
Demo / ESP32 ADC / ADS1115 / UART sources
        |
        v
physical Sensor 1/2/3 --> calibration --> current direction
        |
        v
persisted role mapping --> logical Solar/Load/optional Battery
        |
        v
sensors (validation, buffers, derived values) --> live UI
        v
Measurement/energy aggregation --> persistent history --> historical UI/API
        |
        +--> clock/timeline reconciliation

Network services --> Wi-Fi UI, NTP/browser time, web app/API, OTA, remote display
```

### 1. Sensor acquisition

`sensors/` owns acquisition and exposes thread-safe normalized readings.
`SensorSource` is the physical acquisition boundary. A source returns Sensor
1/2/3 presence, health, voltage/current, and optionally direct duty. ESP32 ADC
and ADS1115 values are raw inputs to source-specific, per-physical-sensor
calibration; Demo and UART values are already engineering units. The service
applies calibration and physical current direction exactly once, derives power
once, maps the readings to logical roles, and owns short logical histories. It
also retains only the latest physical readings for diagnostics, avoiding a
second set of history buffers.

Realtime Demo remains valuable for exercising the complete live/history path.
The ESP32 ADC and external ADS1115 sources are independently build-selectable.
The UART source accepts
calibrated readings and optional duty from an external producer; the current
Uno producer supplies `In`/`Out` while `Aux` is absent.
One UART frame supplies a coherent multi-channel snapshot, so its receiver is a
shared frame source rather than three independent serial readers.

Physical ADC sources use one continuous high-rate acquisition task. Its
source-specific scheduler targets are 5 ms for the built-in ESP32 ADC and
15 ms for ADS1115. Each loop acquires voltage and current together for every
configured physical channel, calibrates those observations, and adds them to
the production reducer. The reducer publishes one `Reading` per channel every
500 ms; normal power, duty, energy, and history consumers continue to use that
stream. ADS1115's nominal conversion rate applies to individual conversions,
not logical samples: the WROOM mapping requires four sequential conversions
for its In and Out voltage/current pairs.

The same task supports a single shared, on-demand diagnostic capture without a
second ADC read path. A request selects one logical channel and retains its
exact calibrated voltage/current/power observations across three consecutive
500 ms reducer windows. Each window also stores the resulting production
reading and duty. Retention begins only at a reducer boundary and ends after
three windows; taking the result releases it. Requests carry monotonically
assigned IDs, take/cancel operations require the matching ID, and abandoned
active/ready results expire. The browser and LVGL Sensors views share this
capture service without allowing a stale consumer to cancel or consume a newer
generation. Captures therefore reflect the acquisition that actually drives
power and energy rather than a separate oscilloscope mode.

Requirements for the production source:

- fixed channel configuration and a clear channel/pin table;
- calibrated millivolt reads, suitable attenuation, and bounded filtering;
- per-source, per-channel persisted offset/gain values with safe defaults;
- independent channel presence and explicit waiting/invalid/stale states;
- finite/plausibility validation for every source, including the local ADC,
  while retaining finite raw observations on Sensors for diagnosis;
- no integration across an unavailable interval and no indefinite reuse of a
  stale last-good value;
- no blocking work while holding the shared sensor-history mutex.

The stable identifiers remain `In`, `Out`, and `Aux` in storage and APIs.
User-facing net power is positive while charging and uses the direct Battery
measurement (`Aux`) when valid, falling back to `In - Out`. Derived metrics
that lack their required channels are unavailable, not zero. Values are never
clamped into the calculation range because that would fabricate power/energy.
See `SENSOR_DATA_POLICY.md` for states, limits, coverage, and calibration
implications.

### 2. UI

On `meter-viewe`, LVGL owns the display and touch UI. `ScreenManager` provides the persistent
top-level tab layout; screens should only consume public service APIs, never
read hardware or files directly.

Current LVGL top-level screens are:

- **Home:** direct-Battery-first charging summary, 30-second net trend, Battery
  voltage, and the immediately applied/persisted manual brightness slider.
- **Usage:** rolling and calendar history whose positive Solar and negative
  Load totals are subdivided by Battery charge/discharge, with signed Balance
  shown as a muted filled segment. Contradictory three-sensor measurements use
  a floating conflict stack that preserves every magnitude. Inferred two-sensor
  history has no Balance series. Normal stacks order Battery charge/discharge
  at zero, then Solar/Load, with any Balance residual at the outer tip.
- **Power:** Solar and Load live metrics, plus Battery and Balance when Battery
  is mapped.
- **Sensors:** raw voltage/current/power and short trend charts for mapped
  logical sensors only.
- **Settings:** nested configuration and diagnostics pages:
  - **Wi-Fi:** station scan/connect, station IP/RSSI, plus local AP control
    and its gateway IP;
  - **Info:** version/build date, current date/time, uptime, network-named IP
    addresses, and signed Internet update status/actions;
  - **Setup:** persisted device ID/hostname (`meter-...`), source selection,
    active channel-state summary, appearance, staged auto day/night brightness,
    and reset controls;
  - **Data:** dataset-filtered, filename-driven segment diagnostics and live RAM state;
  - **Debug:** SDK/chip/reset details, time source, web build, WebSocket
    connections, disjoint internal/PSRAM heap usage and largest free blocks,
    storage, and OTA diagnostics.

Cycle remains available in the browser and latent in LVGL. PWM duty and
surplus presentation is controlled by the controller profile. See
`LATENT_UI_FEATURES.md`.

The VIEWE display is a first-class offline interface. `meter-wroom` remains
web-first but also drives a compact SSD1306 status surface showing network and
high-level sensor state. The SSD1306 and ADS1115 share a serialized Arduino
Wire bus on WROOM. An ADS-only VIEWE build instead uses ESP-IDF legacy I2C port
1, avoiding the VIEWE panel stack's port-0 driver and Arduino Wire's incompatible
driver-ng constructor. Network operations must be asynchronous and must not
stall sampling or rendering.

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
realtime simulation routes to Demo. The Settings -> Data tab switcher is a
transient file-catalog filter only and defaults to the active source on page
entry. Usage and browser history always follow the active source. Protected
Demo fixture files use reserved session zero and fixed past time; they coexist
with ordinary recorded Demo sessions without sliding or overlap. Fixture V2 is
generated from the timestamped `demo-source` capture, spans about 29 hours 45
minutes, retains its one-hour source gap, and initially begins 48 hours before
current time. Seeding/versioning never touches Real.

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

Cycle boundaries follow that same deliberate fixed-offset model. They are
contiguous 24-hour intervals calculated with the currently configured offset;
they do not model 23/25-hour daylight-saving transition days or retain historic
offset rules. Changing the configured offset can therefore shift how older raw
measurements are grouped into local cycles without changing their UTC anchors.

An unanchored boot block may be reconciled when anchors bound both sides. Its
total unexplained downtime is explicit timestamp uncertainty and the block is
shown only where that uncertainty fits the selected Usage bucket, retaining
otherwise valid energy without placing it falsely at now.
While the current boot itself is unanchored, Usage still queries that session
directly in its monotonic domain. Both LVGL and Web show rolling ranges with
relative labels plus a Since Boot view; calendar-only choices appear after an
anchor is available. Older sessions are never compressed onto that relative
axis.
Missing or uncertain coverage is considered user-significant only when it exceeds
one minute; smaller restart gaps remain in the data model but do not raise the UI
warning symbol.

### 5. Network and future services

`network_manager` owns Wi-Fi state, persisted station credentials, AP mode,
and NTP reachability. It must be initialized from startup and ticked by a
well-defined non-UI owner; UI screens issue commands and display state.

The implemented local service layer includes:

- a shared asynchronous energy-cycle summary model used by LVGL and Web;
- realtime power/history plus raw sensor-diagnostic browser APIs. Current V5
  WebSocket frames append physical Sensor 1/2/3 diagnostics for configuration,
  while the 30-second replay remains on the compact logical-only V4 layout;
- an embedded web application with Power, Usage, Cycle, Sensors, Setup, Wi-Fi,
  diagnostics, and capability-gated remote-display views;
- browser time contribution;
- signed OTA delivery and diagnostics;
- remote display capture and input on a trusted local network.

Browser calibration is implemented against the same persisted source profiles
used by LVGL. Data export and synchronization between devices remain future
work.

Mesh networking is a future architectural option, not a near-term dependency;
the first network contract should work over ordinary Wi-Fi AP/station mode.

## Configuration and persistence

Configuration belongs in NVS/Preferences and is versioned. It includes the V1
`ADC | ADS1115 | UART | Demo` sensor source mode, source-specific calibration
values, per-source physical-to-logical mapping/current directions, device
identity, and Wi-Fi/AP settings. Mapping lives in the `sensor_map` namespace;
calibration remains in `sensor_cal` and follows its physical sensor when roles
change. The mapping API is `GET/PUT /api/v1/sensors/mapping`; phase (d) exposes
the contract, and the LVGL and responsive Web Setup editors apply that same
contract for the active source. Both preview physical V/A/W readings, direction
changes, and Balance before saving. Web loads mapping configuration once and
uses the application's existing live socket for subsequent physical readings;
it does not open another socket or poll mapping diagnostics. Applying a mapping
restarts acquisition so logical history cannot mix roles. Source-advertised
presence already flows through the runtime model. No alpha source-mode
compatibility is required. Future peer/service settings are out of the current
phase.

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

- Runtime channel presence, validity/staleness, calculation eligibility, local
  source summary, and browser sensor/UART diagnostics are implemented.
  Persisted per-source local enable masks remain incomplete.
- Candidate History V1 tenant isolation, per-channel coverage, protected Demo
  fixtures, and dataset-scoped reset/retention are implemented but not yet
  validated by the complete history test plan.
- History/time needs deterministic recovery/query tests, accelerated retention
  tests, and real rotation/multi-day verification before it is production-durable.
- The UART source and its versioned protocol build and pass host tests, but the
  current producer has not been tested with the physical Uno and divider.
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
`UART_SENSOR.md`, and `HISTORY_TEST_PLAN.md` for the active delivery
sequence and acceptance work.
