# Memory-pressure follow-up

## Why this exists

During development of the three-sensor Usage visualization on `meter2`, the
physical touch controller temporarily stopped responding and the embedded web
application remained on its small HTML loading shell. The display continued to
refresh. USB, HTTP, framebuffer, and browser diagnostics showed:

- Firmware `0.2.3-b.5` remained alive and did not reboot while touch recovered.
- The CHSC6540 recovery added by commit
  `61df5ba164c531016720dcf3e0a9ca368b31c021` is a plausible explanation for
  the touch recovery, but the firmware does not expose enough touch telemetry
  to prove which recovery path ran.
- Wi-Fi RSSI was a healthy `-47 dBm`.
- Internal 8-bit heap was 93–94% used and its largest free block was only
  7 KiB. PSRAM was about 6% used with a largest free block around 7.5 MiB.
- Small API responses often succeeded, but some requests timed out after five
  seconds. One request for the 49,895-byte compressed JavaScript asset stalled
  for 20 seconds without returning any bytes.
- The HTML shell is only 706 compressed bytes, so it can load while the module
  script stalls and leave the browser displaying “Loading the meter
  interface…” indefinitely.
- The JavaScript received on a successful request exactly matched the local
  build and loaded successfully in a clean Chromium profile. This rules out a
  corrupt embedded asset or deterministic application-startup exception.

The immediate Usage optimization is implemented with three measures:

1. LVGL leases the history worker's completed `PowerBucket` buffer instead of
   copying all 336 buckets into a second static array.
2. The chart retains compact Solar, Load, Battery, and measured/inferred values
   and derives the five floating ranges only while measuring or drawing.
3. The compact chart storage is allocated through `heap_policy`, which places
   it in PSRAM on VIEWE and retains an internal-memory fallback for a future
   non-PSRAM touch target.

The WROOM build excludes `src/ui/`, so the LVGL Usage chart is not linked into
WROOM firmware. Shared services such as `history_query_service` still use
`callocPreferred`: PSRAM on VIEWE, internal RAM on WROOM.

## Optimization verification on meter2

The optimized build was deployed as signed OTA version `0.2.3-b.6` to `app0`.
It confirmed healthy without rollback.

- VIEWE and WROOM firmware builds passed.
- All 38 native firmware tests and all 17 web tests passed.
- The VIEWE ELF contains only the four-byte `chartStorage` pointer. It no
  longer contains the 13,440-byte static chart array or the 37,632-byte
  `completionCb` bucket array.
- Internal heap use fell from 94% to 74%.
- Largest free internal block grew from 7 KiB to 30 KiB.
- PSRAM remained at 6% used with a largest free block around 7.5 MiB.
- Concurrent compressed JavaScript and CSS requests completed in 0.56 and
  0.22 seconds respectively.
- Three clean Chromium profiles loaded and mounted the full application in
  1.4–2.8 seconds without console or network errors.
- Remote navigation, the history query, the optimized floating-range Usage
  chart, and framebuffer capture all worked.
- After rendering Usage, internal heap remained at 74% used with a 30 KiB
  largest block. The LVGL task retained about 4.9 KiB of its 8 KiB stack.

## Measured Usage memory before this optimization

`historical_storage::PowerBucket` is 112 bytes:

- History worker result: `336 * 112 = 37,632` bytes. This is dynamically
  allocated and already prefers PSRAM.
- Previous LVGL completion copy: another 37,632 bytes in internal `.bss`.
- Old six-series chart: `6 * 336 * 4 = 8,064` internal bytes.
- In-progress ten-endpoint range chart:
  `10 * 336 * 4 = 13,440` internal bytes.

Before optimization, the in-progress LVGL Usage screen therefore reserved
51,072 internal bytes between its bucket copy and chart endpoints. The new
compact union is at most `5 * 336 * 4 = 6,720` bytes and is dynamically
PSRAM-preferred. A compile-time assertion prevents it from growing beyond the
old six-series footprint.

## High-priority follow-up

### Bound embedded static-asset writes

`web_api::serveWebAsset()` currently hands the complete compressed asset to
`WebServer::send_P()`. Arduino's `NetworkClient::write()` then owns a single
large blocking write loop. Since `WebServer` handles one client from the main
application loop, a stalled asset can delay all other port-80 work.

Recommended changes:

- Send the header separately and stream PROGMEM in small chunks, likely around
  one TCP MSS (1,460 bytes).
- Apply a total or no-progress timeout similar to
  `http_utils::writeClient()`.
- Stop the client when progress is not possible.
- Record asset-send attempts, bytes, duration, partial writes, and timeouts.
- Test concurrent HTML, JavaScript, CSS, API, and screenshot requests.
- Preserve `Content-Length`, gzip encoding, ETag, immutable caching, and
  connection-close behavior.

### Expand heap telemetry

The current debug response rounds memory to percentages and KiB, which hides
small but important changes. Add:

- Exact total, free, minimum-ever-free, and largest-free bytes for internal
  8-bit RAM, DMA-capable RAM, and PSRAM.
- Fragmentation indicators such as `largest_free / total_free`.
- Allocation-failure counters where available.
- Free/largest values at the end of startup phases: services, network,
  display, LVGL, navigation, and first history query.
- Task stack high-water marks for Arduino loop, LVGL, sensors, history worker,
  WebSocket service, networking/update workers, and any OTA task.
- Current and peak WebSocket client counts and their buffer allocation totals.
- A schema/version field so debug tooling can safely evolve.

Expose these values in `/api/v1/debug` and print a compact snapshot on serial
when a threshold is crossed. Rate-limit threshold logging.

### Add touch diagnostics

Expose enough state to distinguish I2C faults, invalid frames, and the
controller silently returning “no touch” forever:

- Total CHSC6540 reads, successful pressed frames, no-touch frames, failed I2C
  reads, and out-of-range frames.
- Current and maximum fault streak.
- Hardware-reset count, last reset uptime, and reset reason.
- Last valid touch uptime and coordinates.
- Time spent in touch reads and maximum read duration.
- Optional interrupt-pin state if the board wiring is confirmed.

The current recovery detects negative reads and out-of-range coordinates. It
cannot prove that repeated valid I2C frames with a zero touch count represent a
healthy idle controller. Investigate a safe liveness probe or bounded periodic
reset only if hardware testing demonstrates this silent failure mode.

### Establish memory budgets in CI

Generate and retain firmware size/map summaries for both VIEWE and WROOM:

- Internal `.data` and `.bss`.
- PSRAM/extended `.bss`, if used.
- Flash text/rodata and embedded web asset sizes.
- Largest symbols in each RAM section.
- Delta from the target branch or a checked-in budget.

Fail or warn when:

- Internal static RAM grows beyond an agreed small delta.
- A new internal symbol exceeds a per-object threshold.
- Minimum runtime internal heap drops below a safety floor.
- Largest free internal block cannot support expected Wi-Fi/TLS/HTTP work.
- Task stack margin drops below a defined threshold.

The map-file check should specifically catch large function-local `static`
arrays, which otherwise look harmless in source review.

## Broader memory guidance

### Allocation policy

- Keep DMA, Wi-Fi/lwIP, flash/cache, and small latency-sensitive objects in
  internal RAM.
- Put long-lived, large, byte-addressable application data in PSRAM on VIEWE.
- Use `heap_policy::mallocPreferred()` or `callocPreferred()` when an internal
  fallback is safe.
- Use `mallocPsramOnly()` for optional buffers whose internal fallback would
  endanger the system; disable the optional feature cleanly on failure.
- Avoid relying only on Arduino's global external-allocation threshold. Make
  the intended memory class explicit at important allocation sites.
- Check every large dynamic allocation and expose failures diagnostically.
- Avoid allocating large temporary `String` objects in request handlers.
- Reuse bounded scratch buffers when ownership and concurrency are clear.

### Data representation and ownership

- Prefer read-only leases over full-result copies when the producer already
  owns stable storage.
- Keep leases short. The history worker rejects new work while its Usage result
  is leased, so LVGL must derive chart state and release in the same callback.
- Store source values when derived graphical endpoints can be recomputed
  cheaply. This reduces retained memory and prevents duplicated business
  rules.
- Use unions only for genuinely mutually exclusive modes, as with PWM series
  and MPPT source points.
- Add `static_assert` size budgets to large structs and retained buffers.
- Consider narrower/fixed-point formats only after range and precision
  requirements are documented and tested.
- Avoid retaining a full `PowerBucket` when the consumer needs only a few
  fields.

### WROOM compatibility

- Do not assume PSRAM in shared data/network services.
- Keep WROOM's existing internal fallback measured and budgeted; the shared
  37,632-byte history result is significant on that target.
- Consider a smaller WROOM maximum bucket count, paged/streamed responses, or a
  compact query result type if runtime telemetry shows insufficient margin.
- Continue building both `viewe` and `wroom` for changes to shared services.
- Keep browser-side charts out of the device RAM budget: WROOM serves data, but
  does not compile the LVGL chart.

## Suggested test plan

1. Build VIEWE and confirm map output has no
   `usage_screen::completionCb::buckets` or static `chartStorage` array.
2. Confirm the compact chart allocation lands in PSRAM at runtime.
3. Record exact internal free, minimum-free, and largest-block values before
   and after first visiting Usage.
4. Cycle every Usage range repeatedly while querying web Usage and screenshots.
5. Run simultaneous browser JavaScript/CSS loads and API polling.
6. Leave meter2 running through multiple Demo cycles and theme changes.
7. Exercise physical and remote touch while HTTP is under load.
8. Repeat after an OTA update and verify rollback-health behavior.
9. Build and run WROOM tests; exercise its web Usage queries without PSRAM.
10. Add a soak test that records heap and task-stack watermarks over hours,
    failing on monotonic loss or shrinking largest-block trends.

## Acceptance targets to decide

These need project-specific agreement rather than arbitrary hard-coding:

- Minimum free internal heap during normal operation.
- Minimum largest internal free block.
- Minimum-ever heap floor during HTTP asset delivery, screenshots, OTA, and
  history queries.
- Maximum acceptable internal static-RAM growth per change.
- Minimum task-stack margin by task.
- Maximum HTTP asset stall duration and recovery behavior.
- Touch fault/reset rate that should be considered degraded hardware.

Until those thresholds are established, preserve raw byte counts in test logs
so later baselines can be reconstructed.
