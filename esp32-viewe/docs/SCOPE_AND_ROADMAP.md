# Viewe scope and roadmap

## Product definition

Build an independent, touch-screen solar power meter that accurately shows
live system behavior, retains useful energy/history through unreliable power
and time availability, and can later expose/synchronize that information over
Wi-Fi.

The local display and measurement path are the product's foundation. A network
is optional enhancement, never a condition for sensing, energy accounting, or
history retention.

## Confirmed requirements

### Measurement

- Three voltage/current channels on ESP32 ADC GPIO 5–10.
- `In` represents solar input; `Out` represents load output; `Aux` is an
  independent reserved channel.
- Compute voltage, current, power, energy, net battery power (`In - Out`), and
  solar duty/available-power metrics where meaningful.
- Start with common nominal conversion values, then provide per-channel
  offset/factor adjustment equivalent to the Arduino project's calibration.
- Keep simulation available until real hardware is present for integration.

### On-device experience

- Show live, accurate readings and short trend charts on the VIEWE display.
- Provide derived solar/battery status without incorporating Aux by default.
- Show durable historical minute/hour/day views, including honest gaps and
  time uncertainty where wall clock is unavailable.
- Provide local calibration, Wi-Fi, storage, time/status, and diagnostics
  interfaces as appropriate.

### Time, power loss, and history

- Continue recording ordered measurements and energy without NTP/Wi-Fi.
- Treat every boot as a distinct monotonic session and preserve ordering across
  sessions.
- Store time anchors from NTP, browser clients, and eventually peer devices;
  record their source/provenance.
- Never claim a precise wall timestamp that was not known. Permit later
  reconciliation to a reasonable interval/window.
- Record outages as measurement gaps. Energy during an outage cannot be
  reconstructed and must not be fabricated.
- Minimize bounded data loss during sudden power interruption and recover
  safely from incomplete flash writes.

### Connectivity and platform growth

- Work fully offline; Wi-Fi station/AP configuration remains optional.
- Build a local web app after the core meter/history path is dependable.
- Use one firmware for sensor-equipped meters and sensorless peer/display
  devices, with configurable role.
- Leave room for peer data/time synchronization, mesh experimentation, OTA,
  remote diagnostics, and collaborative features.

## Deliberately deferred decisions

- Exact GPIO mapping of voltage/current pairs to `In`, `Out`, and `Aux`.
- Final retention duration, storage capacity budget, and export policy.
- Calibration workflow details and required reference equipment.
- Security/authentication model for web/peer/OTA services.
- Peer discovery and transport choice; mesh networking is exploratory.
- Whether Aux later becomes a configurable second panel or separate load.

## Delivery order

| Priority | Outcome | Work included |
| --- | --- | --- |
| 0 | Clean, coherent baseline | Remove/consolidate dead scaffolding; correct docs/config names; define startup ownership; repair history semantics/retention; keep a passing simulation build. |
| 1 | Accurate local live meter | Integrate ESP32 ADC source, fixed initial mapping, raw-ADC diagnostics, filtering/error states, and verify display readings against hardware. |
| 2 | Durable useful history | Versioned crash-tolerant buckets/counters, boot sessions, time service/anchors, minute/hour/day UI, retention and recovery tests. |
| 3 | Adjustable calibration | NVS-backed per-channel calibration, local touch workflow, reset/defaults, and clear validation/readback. |
| 4 | Web application | Local API, realtime stream, history/query/export, browser time contribution, and web UI based on the MicroPython project's proven ideas. |
| 5 | Multi-device and advanced features | Role configuration, peer time/data sync, OTA workflow, diagnostics, mesh research, and optional realtime collaboration. |

Each priority should end with a buildable firmware, a short manual verification
procedure, and a documented data/configuration migration impact.

## Current status (July 2026)

- Priorities 0 and 1 are substantially implemented: startup ownership is
  explicit, simulation and ADC sources share one sensor pipeline, networking is
  optional, and the project builds from PlatformIO. Physical ADC accuracy still
  requires validation on the installed electrical hardware.
- Priority 2 is substantially implemented. History V3 has boot/session
  segments, fixed 32-byte minute rows, five-minute buffered writes, a bounded
  anchor ledger, rolling/calendar queries, explicit gaps, a file-oriented
  diagnostic screen/API, and Usage reset. Basic build, boot, API pagination,
  and UI rendering are verified; interruption, rotation, and multi-week
  retention testing remain.
- Priority 3 is substantially implemented, including NVS-backed per-channel
  calibration, touchscreen adjustment/preview, reset, and readback.
- Parts of later priorities arrived early: authenticated OTA and remote display
  control work. The full data API/web application and peer synchronization are
  still deferred.

## Implemented milestone: session-aware calendar history

This phase replaced the development history files; backward compatibility was
not required. It includes:

1. a dedicated time service with a persistent boot-session ID, 64-bit monotonic
   time, persisted NTP/browser anchors and their provenance;
2. a fresh segmented history format with session/minute metadata in filenames,
   fixed 32-byte rows, 240-row rotation, and a five-minute PSRAM write buffer;
3. reconciliation of an unanchored block when surrounding anchors bound its
   total uncertainty within the selected graph bucket;
4. an incomplete-data indication only when missing or uncertain coverage exceeds
   one minute, represented primarily by gaps and a small warning symbol;
5. real-time and calendar queries: Last 1/6/24 Hours, Today, Yesterday,
   Last 2 Days (Today + Yesterday), Last Week, Last Two Weeks, and All;
6. a persisted fixed UTC offset for local-day boundaries. Full timezone/DST rule
   support is deliberately deferred;
7. an authenticated browser-time endpoint and paginated file-catalog endpoint.
   Building the complete browser app is a separate milestone;
8. separate internal/PSRAM diagnostics with usage and largest-free-block values,
   plus PSRAM-first LVGL allocation to preserve scarce internal-capability RAM.

Records without a defensible wall-clock mapping remain absent from time-labeled
Usage views rather than being moved to now. Bounded estimates carry an explicit
small inference warning; unresolved intervals remain gaps. Short restart
fragments and sub-minute gaps do not create a user warning.

## Remaining near-term backlog

1. Validate the current sequential ADC map and calibrated readings against the
   installed electrical hardware.
2. Exercise History V3 through repeated real power interruptions, partial flash
   writes, file rotation, and multi-week retention.
3. Revisit whether the five-minute write buffer should eventually become one
   minute for production durability.
4. Build the local data API and Svelte web application, including automatic
   browser time contribution, realtime/history queries and export.
5. Add user-facing fixed UTC-offset configuration if browser contribution is
   not sufficient for deployments outside the default Mountain offset.

## Production-hardening acceptance criteria

- The project builds with PlatformIO from a clean checkout and passes a written
  on-device verification procedure.
- Simulation and ADC modes visibly drive live and historical views without
  sampling or rendering failures.
- Power interruption loses no more data than the selected write-buffer policy,
  and incomplete writes recover without corrupting older records.
- Time-labeled history shows honest gaps, inferred coverage is disclosed, and
  the display raises only the agreed minimal warning.
- Retention and storage usage are measured from actual rotation behavior.
- No UI or history value implies a physical/system meaning that has not been
  specified and hardware-validated.
