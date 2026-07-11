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

## Immediate cleanup backlog

1. Define the single `sensors` pipeline and remove or isolate unused sensor
   scaffolding.
2. Replace hard-coded simulated construction with an explicit selectable
   simulation/ADC configuration path.
3. Establish the ADC pin/channel map and electrical validation plan.
4. Define a history record format that carries session/order and time-anchor
   metadata; correct retention rotation before treating existing data as durable.
5. Rewrite the Usage chart from agreed energy/bucket semantics rather than
   provisional channel indexes and formulas.
6. Make network initialization/update ownership explicit; document its
   optional/offline behavior.
7. Reconcile partition/filesystem terminology and remove unreferenced UI/API
   fragments.

## Acceptance criteria for the next implementation phase

Before declaring Priority 0 and moving into hardware integration:

- The project builds with PlatformIO from a clean checkout.
- Simulation mode visibly drives all intended live views and historical sample
  capture without crashes.
- The active sensor, storage, network, and screen ownership are each
  unambiguous in code and documentation.
- Stored history has a verified retention calculation and a documented
  power-loss/recovery behavior.
- No UI or history value implies a physical/system meaning that has not been
  specified.

## Open question required before ADC implementation

Please specify the exact pairing/order for GPIO 5, 6, 7, 8, 9, and 10—for
example, `In voltage=5, In current=6, Out voltage=7, ...`. The architecture
does not assume an ordering so that an incorrect mapping cannot be silently
encoded.
