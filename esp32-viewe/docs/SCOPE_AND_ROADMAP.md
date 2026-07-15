# Viewe scope and roadmap

## Product definition

Build an independent touchscreen solar power meter that accurately shows live
system behavior, retains useful energy history through unreliable power and
time availability, and exposes that information over an optional local network.

The local measurement, display, and history paths are the product foundation.
Wi-Fi must never be required for sensing, energy accounting, or retention.

## Product requirements

### Measurements and sensor sources

- Support the logical `In`, `Out`, and `Aux` voltage/current channels.
- Treat channel presence independently. A device may provide any subset of the
  three channels; an absent, invalid, or stale channel is not zero power.
- Compute power and energy on the ESP32 from normalized engineering-unit
  readings. `In - Out` is net battery power when both channels are available;
  `Aux` remains independent.
- Accept readings from multiple source implementations behind one contract:
  realtime simulation, the ESP32 ADC, and an interim Arduino Uno UART bridge.
- Carry source health, sample age, channel presence, and optional direct duty
  cycle with every source. Validate finite and physically plausible values for
  all sources, including the local ADC, while preserving finite raw
  observations for diagnostics.
- Apply calibration exactly once. The ESP32 applies its persisted calibration
  to raw ADC input; UART readings arrive already calibrated in volts and amps.
- Preserve enough source provenance and diagnostics to support a future remote
  sensor source without designing that network in the current phase.

### On-device and browser experience

- Show current readings, source health, derived power/battery status, and short
  trends on the VIEWE display.
- Show durable minute/hour/day history with honest gaps and explicit time
  uncertainty where wall time is unavailable.
- Provide local calibration, Wi-Fi, storage, time/status, setup, and diagnostics.
- Serve a useful local web application with live readings, history, browser
  time contribution, device information, and remote-display control.
- Add browser calibration/settings/storage features as they become useful;
  they are product backlog, not blockers for the completed web-app milestone.

### Time, power loss, and history

- Record ordered measurements and energy without NTP or Wi-Fi.
- Treat every boot as a distinct monotonic session and preserve ordering across
  sessions.
- Store time anchors from NTP and browser clients with source/provenance.
- Never claim a wall timestamp that was not known or defensibly bounded.
- Record outages and invalid/out-of-range/stale sensor intervals as gaps. Do not
  fabricate energy during an outage or integrate the last good sample forever.
- Bound data loss during sudden power interruption and recover safely from
  incomplete flash writes.
- Keep realtime simulation suitable for exercising the production history
  path. Synthetic pre-populated showcase history must be a separate, explicit
  facility and must not interfere with normal history verification.

## Milestone status

"Implemented" means the feature is present and builds. "Verified" means its
documented software acceptance checks have passed. Physical electrical
accuracy is deliberately tracked in a later on-site milestone.

| Milestone | Status | Remaining work |
| --- | --- | --- |
| 0. Coherent firmware baseline | Verified | Ongoing documentation maintenance only. |
| 1. Local live meter and source abstraction | Implemented | Production-ready null/stale/error semantics and independent channel presence are active work. |
| 2. Session-aware durable history alpha | Proven feasibility | Replace the disposable alpha format with candidate V1, then verify rotation, retention, interruption recovery, and multi-day accuracy. |
| 3. Local calibration | Implemented | Electrical calibration and accuracy validation belong to the on-site hardware milestone. |
| 4. Embedded web application | Complete | Calibration/settings/storage pages and appearance refinements are normal backlog. |
| 5. OTA and remote display | Verified for current workflow | Continue regression coverage as related services change. |

The current ESP32 firmware and embedded web app build successfully. The alpha
history service, live and historical browser views, Wi-Fi/AP setup,
browser/NTP time anchors, local calibration, signed OTA, diagnostics, and
remote display control are in place.

## Active software milestones

### A. Production sensor data contract

Make missing and unreliable sensor data explicit before adding another source.

Included work:

- represent channel presence separately for `In`, `Out`, and `Aux`;
- persist source-specific enabled masks; a fresh ADC configuration enables no
  channels until the installer identifies attached hardware;
- distinguish waiting, valid, invalid, and stale source states;
- retain sample/source timestamps and expose last-valid age and error counters;
- reject non-finite and configurable implausible readings from every source;
- show unavailable values as unavailable in LVGL, web, derived metrics, and
  history rather than coercing them to zero;
- stop energy integration across missing/stale intervals and create honest
  history gaps;
- migrate the persisted Demo/Real boolean to a versioned source-mode enum while
  preserving existing devices' settings.

Acceptance requires focused tests for partial channel configurations, source
startup, malformed values, timeout/recovery, mode migration, derived metrics,
and history gaps.

The initial calculation limits are 0–120 V, -50–50 A, and 0–1 direct duty.
Sensors shows finite observations outside those limits with a warning; Power
and history reject rather than clamp them. See
[SENSOR_DATA_POLICY.md](SENSOR_DATA_POLICY.md).

### B. History verification and demo separation

Candidate History V1 keeps two isolated logical datasets using one storage and
query engine:

- **Real:** ADC and UART recordings only.
- **Demo:** realtime Demo recordings plus protected prebuilt fixture files.

Source provenance automatically selects the recording dataset. Settings ->
History `Real | Demo` is only a view filter and never installs, removes, or
reclassifies files. Protected Demo fixtures use reserved session zero and a
fixed past anchor so they cannot slide into and double-count recorded Demo
sessions. Real and Demo have independent retention/reset boundaries.

Exercise candidate History V1 using deterministic file/query tests, accelerated
on-device fixtures where safe, and a real-duration hardware soak. Cover torn
rows, stale `.open` files, repeated boots, 240-row rotation, 200-file eviction,
bounded time inference, gaps, and independently calculated multi-day totals.
See [HISTORY_TEST_PLAN.md](HISTORY_TEST_PLAN.md).

No alpha history compatibility is required. Candidate firmware may wipe the
single development device and begins the production format at V1. See
[HISTORY_STORAGE_V1.md](HISTORY_STORAGE_V1.md).

### C. Arduino Uno UART sensor

Use the installed `arduino-lcd` meter as an interim real-world source while the
new ADC hardware is unavailable.

Included work:

- modernize `arduino-lcd` with a reproducible PlatformIO Uno build while
  retaining Makefile compatibility;
- preserve its LCD, buttons, calibration, EEPROM, energy, and sampling behavior;
- emit a bounded versioned record at 2 Hz, independent from its faster sampling;
- transmit calibrated engineering units, channel presence, sequence/uptime,
  and Arduino-computed duty; `Aux` is normally absent;
- receive and validate records on VIEWE J4 UART0 RX/GPIO44 without blocking;
- add a UART sensor mode and source diagnostics to the local and web interfaces;
- let the ESP32 calculate power and integrate its own energy/history.

The electrical connection and wire contract are specified in
[ARDUINO_UART_SENSOR.md](ARDUINO_UART_SENSOR.md). Because the Uno is not
currently available for bench testing, its changes require reviewable host-side
protocol tests and a conservative flash/on-site checklist.

## Future on-site hardware integration

This is a separate milestone, not a blocker for completing the current software
work. Begin it when the final sensor assembly and installed system are available.

- Confirm the final ADC GPIO/channel/polarity map and connector wiring.
- Verify every ADC input remains within the ESP32 electrical limits.
- Compare raw millivolts and converted readings against trusted instruments
  across expected voltage/current ranges.
- Establish per-channel defaults, calibration procedure, filtering, plausible
  limits, noise behavior, and disconnected-input behavior.
- Verify power, duty/available power, energy, and net battery calculations.
- Run an on-site soak through normal charging/load cycles and power events.
- Document the final hardware revision, reference equipment, results, and any
  configuration migration.

## Deferred/out of scope

- Remote sensor transport, peer discovery, mesh networking, aggregation, and
  duplicate-source resolution.
- Alternate ESP32-WROOM/OLED firmware targets.
- A cluster-wide web view. The current app is complete for one meter; a future
  aggregator must first define ownership and provenance for peer data.
- Automatic detection of attached ADC sensors. Floating inputs are not reliable
  evidence of presence; channel assignment remains explicit configuration.
- Full IANA timezone/DST rules, internet-facing services, and a final remote
  authentication model.

## Delivery requirements

Each active milestone ends with:

- clean PlatformIO builds for affected firmware;
- focused automated or deterministic tests where hardware is not required;
- a short manual verification and recovery procedure;
- documented persisted-data/configuration migration impact;
- no UI, API, or history value that implies unavailable or unverified physical
  meaning.
