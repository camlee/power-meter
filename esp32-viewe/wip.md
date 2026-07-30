# Power and energy sensor rework

This file tracks the agreed incremental work. Each phase is implemented,
verified, and flashed to `meter2` before the next phase begins.

## Confirmed assumptions

- Physical/default order: Sensor 1 = Solar, Sensor 2 = Load, Sensor 3 = Battery.
- The ESP32 ADC pin definitions are correct; stale comments are not.
- Preserve the ESP32 ADC calibration performed at commit `3274aed`.
- Battery current is intended to be positive while charging.
- When Battery is unavailable, Battery voltage follows Load voltage.
- Normal Usage remains a stacked chart whose positive total is measured Solar
  and whose negative total is measured Load.

## Phases

- [x] **(a) Reproducible Demo baseline**
  - Realtime Demo cycles through deterministic scenarios on one-minute
    monotonic boundaries.
  - Usage exposes stored one-minute precision through Last 1 Hour.
  - The protected historical fixture retains captured two-sensor data and
    legacy inferred Battery components.
  - Existing Usage/sign behavior is intentionally unchanged for comparison.
- [x] **(b) Correct current comments and obvious presentation/domain logic**
  - Correct stale comments, fix Usage stacking, rename Error to Balance, and
    replace the Power-screen Net series with Balance.
- [x] **(c) Apply the intentional default sensor-direction correction**
  - Make the installed Battery reading positive while charging without
    changing physical calibration.
- [x] **(d) Separate physical sensors from logical roles**
  - Preserve feature parity and formally support an unmapped Battery.
- [x] **(e) Add LVGL sensor-mapping UI**
- [x] **(f) Add Web sensor-mapping UI**
- [ ] **(g) Final UI refinements**
  - Balance visibility controls and final interaction/presentation cleanup.

## Phase (a) verification oracle

Realtime Demo repeats this five-minute cycle. Power values use the current
pre-refactor contract: Solar and Load are positive magnitudes; Battery is
positive for charge and negative for discharge.

| Time | Scenario | Solar | Load | Battery | Current Balance formula |
| --- | --- | ---: | ---: | ---: | ---: |
| 0–1 min | Day charge | 40 W | 14 W | +26 W | 0 W |
| 1–2 min | Solar direct | 18 W | 18 W | 0 W | 0 W |
| 2–3 min | Night discharge | 0 W | 16 W | -16 W | 0 W |
| 3–4 min | Balance mismatch | 20 W | 6 W | +22 W | -8 W |
| 4–5 min | Discharge conflict | 6 W | 20 W | -22 W | +8 W |

The values have a deterministic ±1.5% shared ripple so live charts move while
the energy relationship remains unchanged.

The protected historical fixture is generated from captured logs `0` through
`29`: approximately 29 hours 45 minutes of two-sensor input/output data. It
contains 1,725 data minutes and one explicit 60-minute gap in its 1,785-minute
span. Battery charge/use remains inferred exactly as the legacy implementation
did. The original capture is intentionally untracked; validate a local copy
with:

```sh
python3 tools/build_demo_profile.py \
  --source ../esp32-arduino/demo-source \
  --check
```

### Phase (a) verification — 2026-07-29

- Historical generator reproduced 119 profile points: 1,725 covered minutes,
  eight protected segments, and the explicit 60-minute gap.
- Native tests passed: 28/28, including three Demo-schedule tests.
- The `viewe` firmware build passed.
- Signed OTA installed on `meter2`; it is confirmed healthy in `app1` with no
  rollback.
- The live sensor API observed all four scenarios and their expected values
  across one complete cycle.

Deployment note: `tools/ota.py --version` applied the requested version to the
signed manifest but not to the embedded build version, causing its wait step to
time out even though the device subsequently confirmed the image. That first
deployment identified itself as `0.2.2-2.dirty+g87a0f03`; the refinement below
sets the embedded version explicitly.

### Phase (a) one-minute refinement — 2026-07-29

- Added a one-minute rolling Usage view for deterministic Demo verification.
- Demo scenarios now change on exact boot-monotonic minute boundaries and
  repeat every four minutes.
- Native tests passed: 28/28. Web tests passed: 9/9. The VIEWE build passed.
- Signed OTA version `0.2.3-a.2` is confirmed healthy on `meter2` in `app0`
  with no rollback.
- The device API returned exactly 20 one-minute buckets over a 20-minute span.
- Live verification observed Solar Direct before the 120-second boundary and
  Night Discharge immediately after it.

### Phase (b) verification — 2026-07-29

- One shared power-flow decomposition now enforces both Usage stack totals:
  Charge + Solar remainder equals measured Solar, and Load remainder +
  Discharge equals measured Load.
- Signed Balance (`Solar - Load - Battery`) is a muted filled segment. Normal
  stacks retain measured Solar/Load endpoints; contradictory readings use a
  floating conflict stack so no magnitude is clipped. Inferred two-sensor
  history omits Balance and its legend.
- Normal segment order follows the original history UI from zero outward:
  Charge then Solar then Balance, and Discharge then Load then Balance.
- Realtime Demo adds a mirrored `+8 W` discharge-conflict minute so both
  floating conflict directions are visible within Last 1 Hour.
- Power now shows Balance instead of its previous duplicate-Battery Net series
  on LVGL and Web. Home and Cycle retain their distinct charging/net meanings.
- Confirmed physical order and GPIO comments now match Sensor 1 / Solar,
  Sensor 2 / Load, and Sensor 3 / Battery. Pins and calibration were unchanged.
- Native tests passed: 38/38. Web tests passed: 17/17. The protected Demo
  fixture check, embedded web build, and VIEWE firmware build passed.
- Signed OTA version `0.2.3-b.5` is confirmed healthy on `meter2` in `app1`
  with no rollback; Demo mode and the new embedded web build are active.

### Phase (b) memory and presentation refinement — 2026-07-29

- Usage retains compact Solar/Load/Battery sources in PSRAM, derives the five
  ranges on demand, and briefly leases the history worker result instead of
  retaining duplicate bucket and endpoint arrays.
- Removed the temporary full-bar outlines from LVGL and Web.
- Web labels are Battery Charging, Solar In, Solar Usage, Battery Usage, and
  Balance. The smaller LVGL legend uses Charge, Solar In, Solar Use, Bat Use,
  and Balance on one row.
- Removed Last 20 Minutes. Last 1 Hour now uses 60 one-minute buckets, and Last
  6 Hours uses 36 ten-minute buckets.
- Native tests passed: 38/38. Web tests passed: 17/17. The protected Demo
  fixture check, embedded web build, and both VIEWE and WROOM firmware builds
  passed.
- Signed OTA version `0.2.3-b.7` is confirmed healthy on `meter2` in `app1`
  with no rollback. Both revised rolling ranges were rendered on-device while
  internal heap remained at 74% used with a 30 KiB largest free block.

### Phase (c) verification — 2026-07-29

- ESP32 ADC Battery current is reversed after calibration, making the intended
  charging direction positive; Battery power follows the corrected current.
- Raw ADC inputs, saved gain/offset calibration, Solar, and Load are unchanged.
  Demo, UART, and ADS1115 retain their existing polarity until the physical to
  logical sensor refactor.
- LVGL and Web calibration previews and trusted-reference gain calculation use
  the same effective polarity as live readings.
- Native tests passed: 41/41. Web tests passed: 20/20. Embedded Web and both
  VIEWE and WROOM firmware builds passed.
- Signed OTA version `0.2.3-c.1` is confirmed healthy on `meter2` in `app0`
  with no rollback. Demo mode remains active with direction multipliers of
  `+1` for all three channels; internal heap is 73% used with a 30 KiB largest
  free block.

### Phase (d) verification — 2026-07-30

- ADC, ADS1115, UART, and Demo now acquire physical Sensor 1/2/3 channels.
  Source-specific versioned NVS profiles map those channels to logical Solar,
  Load, optional Battery, or Unmapped and apply current direction after
  physical calibration.
- Defaults preserve Sensor 1 = Solar, Sensor 2 = Load, Sensor 3 = Battery.
  ESP32 ADC Sensor 3 retains the phase-(c) reversed direction; Demo, UART, and
  ADS1115 retain normal directions. Existing calibration storage is unchanged
  and follows the physical sensor when roles move.
- Logical histories and wire/storage channel IDs remain compatible. Only the
  latest physical readings are retained for diagnostics, avoiding duplicate
  history buffers. An unmapped Battery uses existing Solar-minus-Load power
  inference and Load-voltage fallback behavior.
- `GET/PUT /api/v1/sensors/mapping` exposes and persists the complete active
  source profile. The existing logical sensor API now identifies its mapped
  physical sensor, and calibration accepts both logical and physical IDs.
- Native tests passed: 44/44. Web tests passed: 20/20. The protected Demo
  fixture, embedded Web build, and both VIEWE and WROOM firmware builds passed.
- Signed OTA version `0.2.3-d.2` is confirmed healthy on `meter2` in `app0`
  with no rollback. A persisted test profile swapped Solar/Load and unmapped
  Battery; logical values followed the physical assignments, Battery became
  explicitly not configured, and system net power fell back to Solar minus
  Load. The default Demo profile was then restored and reconfirmed after
  restart. Internal heap settled at 72% used with a 30 KiB largest free block.

### Phase (e) verification — 2026-07-30

- Setup now shows the active source and logical sensor validity beside an edit
  button. Mapping stays disabled while a different, unsaved source is pending.
- The full-screen editor assigns Sensor 1/2/3 to Solar, Load, Battery, or
  Unmapped and independently selects each current direction. Solar and Load
  must each be assigned exactly once; Battery remains optional.
- Each row displays its live physical voltage, draft-directed current, and
  draft-directed power in aligned columns. A live Balance below the rows uses
  the complete draft mapping and direction choices.
- Cancel discards the draft. Save persists the active source profile and
  restarts the meter so historical logical roles cannot change mid-session.
- Native tests passed: 44/44. The embedded Web and VIEWE firmware builds
  passed. On-device draft reversal immediately changed the displayed current,
  power, and Balance without saving. Opening the editor left internal heap
  usage and its 30 KiB largest free block unchanged.
- Signed OTA version `0.2.3-e.4` is confirmed healthy on `meter2` in `app0`
  with no rollback.

### Phase (e) display refinement — 2026-07-30

- The mapping actions are anchored to the bottom of the display with larger
  touch targets.
- Live V/A/W diagnostics use larger 20 px type and one decimal place.
  Dropdowns are taller, use larger text, and explicitly center their contents.
- Setup left-aligns the active source/status text and presents mapping as a
  plain edit icon instead of a filled button.
- Native tests passed: 44/44. Web tests passed: 20/20. Firmware version
  `0.2.3-e.5` was flashed to `meter2` over USB, and esptool verified every
  written image.
- Live mapping diagnostics now refresh every two seconds. V and A each use a
  narrower 90 px column, leaving 114 px for W. The three W rows consistently
  switch to whole watts when all are at least 10 W in magnitude or any reaches
  1000 W, preventing large values from wrapping.
- Firmware version `0.2.3-e.6` was flashed to `meter2` over USB, and esptool
  verified every written image.
- The mapping overlay now uses a left-side back arrow, a readable filled
  Cancel action, and an explicitly centered `Save & Restart` action. Its
  optional role is labeled `None`.
- Balance is a diagnostic row aligned with the W column. Its help text reports
  unaccounted power as a percentage of the largest mapped sensor reading, or a
  short reason when the mapping/readings cannot produce Balance.
- Setup separates the active source at left, logical sensor status in the
  middle, and the edit icon at right.
- Native tests passed: 44/44. Firmware version `0.2.3-e.8` was flashed to
  `meter2` over USB, and esptool verified every written image.
- Setup now renders the mapping pencil in black. The polarity controls use
  `Current` with up/down arrows for normal/reversed direction.
- Firmware version `0.2.3-e.9` was flashed to `meter2` over USB, and esptool
  verified every written image.
- The Setup pencil now uses the same theme-aware muted gray as Sensors-page
  calibration icons. The mapping header uses separate `Sensor mapping` and
  active-source rows, and its action is labeled `Save & Reboot`.
- Firmware version `0.2.3-e.10` was flashed to `meter2` over USB, and esptool
  verified every written image.
- Each physical sensor now uses a `+ / -` segmented polarity control. A small
  draft-aware row interprets the resulting power as Producing, Consuming,
  Charging, Discharging, Idle, Not mapped, or a polarity warning.
- Sensor validity icons moved beside the V/A/W diagnostics. Valid mappings no
  longer show redundant clean/dirty helper text; validation errors remain.
- Firmware version `0.2.3-e.11` was flashed to `meter2` over USB, and esptool
  verified every written image.
- Role and polarity changes now refresh V/A/W, Balance, and interpretations
  immediately; passive reading updates retain the two-second cadence.
  Interpretation text is prefixed with its draft logical role.
- Firmware version `0.2.3-e.12` was flashed to `meter2` over USB, and esptool
  verified every written image.
- Optional-Battery presentation is now consistent: Power omits Battery and
  Balance KPIs when Battery is unmapped, Sensors omits unmapped logical tabs,
  and Home presents the Load-voltage fallback simply as Battery voltage.
- Mapping persistence failures explicitly reveal their validation error
  instead of leaving it hidden.
- Native tests passed: 44/44. Web tests passed: 20/20. The protected Demo
  fixture and VIEWE firmware build passed. Firmware version `0.2.3-e.13` was
  flashed to `meter2` over USB, and esptool verified every written image.

### Phase (f) verification — 2026-07-30

- Web Setup now mirrors the LVGL sensor summary below source selection and
  opens mapping as a responsive inline panel: right of Setup on desktop and
  directly below the summary on phones.
- Each physical sensor has a role selector, `+ / -` current-direction control,
  state, aligned V/A/W diagnostics, and draft-aware interpretation. Balance,
  validation, Cancel, and Save & Reboot follow the same contract as LVGL.
- Mapping configuration is fetched once when the editor opens. Physical
  readings arrive at 2 Hz through the application's existing WebSocket; role
  or direction edits recalculate readings and Balance immediately without
  polling or opening another connection.
- Live protocol V5 appends physical diagnostics to the current frame. The
  60-frame replay remains logical-only V4, limiting the firmware's additional
  internal static RAM to 40 bytes instead of widening the 5.04 KiB ring.
- Native tests passed: 44/44. Web tests passed: 29/29, including V4/V5 parsing,
  mapping validation, polarity preview, interpretation, and Balance. The Web
  asset check, protected Demo fixture, and both VIEWE and WROOM builds passed.
  WROOM remains within its application partition at 96.3% flash use.
- Firmware version `0.2.3-f.1` was flashed to `meter2` over USB, and esptool
  verified every written image. The running device reported the matching
  version/Web build and emitted 60 compact V4 replay frames followed by a
  128-byte V5 frame with valid physical Sensor 1/2/3 readings.
- Web mapping consistently presents physical sensors as rows at every
  breakpoint. Setup enters mapping through a labeled `Remap` button, and its
  primary action now says `Save & Reboot`, matching the mapping editor and
  touchscreen terminology.
- Web tests passed: 29/29 and the embedded asset check passed. Firmware version
  `0.2.3-f.2` was flashed to `meter2` over USB; esptool verified every image,
  and the running device reported the matching version and Web build.
- Web sensor rows now place the physical name, role dropdown, and `+ / -`
  direction control together on one line. The redundant mapping-reboot helper
  sentence was removed; the `Save & Reboot` action remains explicit.
- Web tests passed: 29/29 and the embedded asset check passed. Firmware version
  `0.2.3-f.3` was flashed to `meter2` over USB; esptool verified every image,
  and the running device reported the matching version and Web build.
- Web mapping now follows the LVGL row hierarchy on phone and desktop: a shared
  Sensor/Role/Current direction heading, a non-wrapping control row, then
  validity plus aligned V/A/W readings and the logical interpretation.
- Removed the web-only border, background, and padding around the full mapping
  panel while retaining the individual sensor-row grouping used by LVGL.
- Web tests passed: 29/29 and the embedded asset check passed. Firmware version
  `0.2.3-f.4` was flashed to `meter2` over USB; esptool verified every image,
  and the running device reported the matching version and Web build.
