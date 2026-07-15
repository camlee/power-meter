# History V1 candidate verification plan

## Purpose

The alpha history implementation has accumulated plausible data on the physical
VIEWE device, but candidate V1 changes the format and adds dataset isolation and
per-channel coverage. Production durability requires repeatable evidence beyond
a normal soak. This plan separates deterministic software verification,
accelerated on-device cases, and real-duration observation.

The normative format and recovery rules remain in
[HISTORY_STORAGE_V1.md](HISTORY_STORAGE_V1.md). Candidate user-visible behavior is
described in [TIME_AND_HISTORY.md](TIME_AND_HISTORY.md).

## Test data policy

Realtime Demo readings use the same sensor, energy integration, minute
aggregation, rotation, and query path as physical readings, but are routed to
the Demo dataset. They are valid test input and create ordinary nonzero-session
Demo history.

Pre-populated Demo session-zero files are protected fixtures, not recorded
measurements. They are visibly identified and excluded from Real acceptance
results. Their fixed anchor must not slide or overlap recorded Demo sessions.
Tests select their dataset explicitly and never infer it from sensor mode.

Never use floating ADC values as an accuracy oracle. Real ADC mode can exercise
storage cadence and interruption, but only deterministic samples or independent
reference calculations can establish energy accuracy.

## Test layers

### 1. Deterministic format and query tests

Run without waiting for wall time by constructing known candidate V1 records, filenames,
sessions, and anchors through test-only helpers or a host-side harness.

- Strictly accept valid `.open` and `.bin` names and reject malformed names.
- Reject alpha V3 names/data after the documented one-time wipe boundary.
- Require directory and filename dataset identity to agree.
- Ignore incomplete trailing `.open` bytes without damaging complete rows.
- Detect/report a `.bin` filename count that disagrees with file size.
- Read stale `.open` files across later boot sessions.
- Rotate exactly at 240 complete rows without overwriting an existing target.
- Enforce a separate 200-file cap for each dataset, never deleting the active
  file or protected Demo fixtures and never evicting across datasets.
- Compact anchors that no retained file in either dataset references.
- Query rolling and calendar boundaries with direct, inferred, and missing time.
- Preserve time gaps, per-channel/component coverage, configured masks, and
  inference flags through bucket aggregation.
- Calculate multi-day Wh totals from known sample power and elapsed intervals,
  including partial query boundaries.
- Omit unavailable channels without turning them into zero energy or preventing
  valid channels from being retained; distinguish measured zero, partial
  coverage, configured-but-missing, and not-configured.
- Route Demo source records only to Demo and ADC/UART records only to Real.
- Verify source capability/advertised masks intersect source-specific enabled
  masks and floating ADC pins cannot create history while disabled.
- Keep history view filtering independent from recording destination.
- Pin fixture time once, delay it when recorded Demo time is unresolved, and
  prove fixture and recorded Demo intervals do not overlap or double-count.

Tests must use a temporary/test filesystem or an explicitly disposable device
dataset. Production constants and on-device data are not changed merely to make
tests faster.

### 2. Accelerated on-device verification

Use a test-only fixture/import path to install crafted files on the ESP32, then
exercise the real LittleFS catalog, query service, LVGL Usage view, diagnostics,
and browser API.

- Catalog and display a mixture of closed, interrupted, malformed, directly
  anchored, inferred, and unresolved segments in both datasets.
- Query at least 1 hour, 24 hours, Today, Yesterday, Last 2 Days, and All.
- Confirm API and LVGL totals/flags agree for the same range.
- Fill each dataset beyond 200 files and verify deterministic tenant-local
  eviction plus protected Demo fixtures.
- Exercise `Reset Real`, `Reset Demo`, and `Reset All`; verify their file,
  fixture, anchor, RAM, and temporary-file boundaries.
- Reboot after fixture installation and confirm the same retained catalog and
  totals are reconstructed.

Any fixture/import endpoint must be absent from a normal production build or
require an explicit local test build plus authentication.

### 3. Physical interruption tests

Use realtime Demo input for deterministic-looking nonzero power, ensure wall
time is anchored, and record the exact interruption time externally.

- Interrupt before the first complete minute: no fabricated row/file.
- Interrupt with one to four completed rows buffered: bounded expected loss and
  older committed rows intact.
- Interrupt immediately before, during where practically inducible, and after a
  five-row append.
- Reboot repeatedly so several stale `.open` sessions accumulate.
- Leave the device off for more than one minute and confirm a visible gap rather
  than carried-forward energy in the source-derived dataset only.
- Interrupt/recover a sensor source without rebooting and confirm the same gap
  semantics once null/stale sensor handling is implemented.

For every case capture firmware version, source mode, session/file catalog
before and after, expected loss window, observed graph/API result, and serial or
USB-JTAG diagnostics.

### 4. Real-duration soak and accuracy

- Run continuously through at least one natural 240-minute rotation.
- Continue across several local midnights and at least one reboot/outage.
- Record a deterministic Demo power schedule in the Demo dataset or independent
  reference samples in Real.
- Compare per-minute integration, daily Wh totals, and multi-day totals with an
  independently calculated reference, using an agreed tolerance.
- Confirm per-dataset file counts, storage bytes, active/buffered rows, and query
  time remain bounded as data grows.
- Keep a longer-running device active toward natural retention behavior even
  after accelerated eviction tests pass.

The required numeric tolerance should be selected when the deterministic input
schedule and integration rounding are specified. Hardware sensor accuracy is a
separate on-site milestone.

## Evidence and completion

Record results in a dated verification report rather than expanding this plan
with run-specific observations. History software verification is complete when:

- deterministic format/query cases pass reproducibly;
- accelerated on-device catalog, eviction, reset, and UI/API cases pass;
- interruption results never corrupt previously committed records and stay
  within the documented loss window;
- one natural rotation and a multi-day accuracy comparison pass;
- known limitations and the five-minute durability policy are explicitly
  accepted or changed.
