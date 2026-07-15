# Time and history V1 candidate behavior

This is the user-visible behavioral overview for the next history
implementation. The exact candidate file, tenant, coverage, retention, and
recovery contract is [HISTORY_STORAGE_V1.md](HISTORY_STORAGE_V1.md). The current
alpha files called V3 require no compatibility and may be wiped.

## Timeline behavior

Every boot receives a persistent nonzero monotonic session ID. Measurement
order remains available without Wi-Fi or wall time: each segment filename
contains dataset, session, and first complete monotonic minute; each following
fixed-size row represents the next complete minute.

NTP and the browser submit the same anchor relationship:

- boot session ID;
- session-local monotonic instant;
- Unix UTC milliseconds;
- fixed local UTC offset;
- source and declared uncertainty.

The bounded LittleFS anchor ledger retains one useful anchor per nonzero boot
session, independent of the history dataset being viewed. Routine clock
refreshes do not rewrite it unless precision is materially better. A wholly
unanchored block is placed only when anchored data surrounds it and its
unexplained downtime fits the selected graph bucket. Reserved session-zero Demo
fixtures have separate fixed-anchor metadata.

Rolling and calendar ranges leave unresolved time empty rather than moving data
to now. Calendar views use a persisted fixed UTC offset and provide Today,
Yesterday, Last 2 Days, Last Week, Last Two Weeks, and All. Full timezone/DST
rules remain future work.

Time uncertainty and measurement availability are independent:

- an anchored minute can still have a sensor-coverage gap;
- a fully measured minute can still have inferred wall time;
- configured zero power has coverage and zero energy;
- an absent channel is unavailable, not zero and not a sensor failure.

## Browser time contract

The embedded same-origin web app submits time when it connects:

```http
POST /api/v1/time/anchor
Content-Type: application/json

{"unix_ms":1783890123456,"utc_offset_minutes":-360}
```

In JavaScript, use `Date.now()` and
`-new Date().getTimezoneOffset()`. Offset is optional and must be within plus or
minus 14 hours.

## Real and Demo history views

Real and Demo are separate logical datasets using the same format and query
engine.

- ADC and UART sources automatically record to Real.
- Realtime simulation automatically records to Demo.
- Settings -> Data provides a tabbed `Real | Demo` file-catalog filter.
- The filter never installs, removes, copies, or reclassifies data.
- It defaults to the active sensor source whenever the page is entered and is
  reset when the page is left, making a non-active view intentional.
- Usage and browser history always follow the active sensor source and expose no
  independent dataset selector.

The Demo dataset contains protected session-zero fixture files plus normal
recorded Demo sessions. Fixture V2 is generated from the timestamped
`demo-source` capture: it spans about 29 hours 45 minutes and preserves one
missing source hour as an explicit gap. On a fresh dataset its start is pinned
once at approximately 48 hours before current time. If that would overlap a
resolved recorded Demo interval, it moves earlier; if recorded Demo time is
unresolved, anchoring waits. Queries never sum fixture and recorded coverage
for the same interval. New Demo files appear normally when the Demo source runs
and Demo data is viewed.

Candidate firmware idempotently creates/updates only the protected fixture
version. Firmware flash or OTA never removes or rewrites Real history. Demo
reset removes recorded Demo sessions and restores its protected fixture set;
Real reset affects only Real.

## Measurement availability

The Sensors screen shows finite observed engineering values even when they are
outside calculation limits, clearly marked. Power and Usage consume only fresh,
finite values within the bounds in
[SENSOR_DATA_POLICY.md](SENSOR_DATA_POLICY.md):

- voltage 0–120 V inclusive;
- current -50–50 A inclusive;
- direct duty 0–1 inclusive.

Values are rejected from calculation, never clamped. History keeps valid energy
and coverage independently for each channel/component. Source outage, stale
input, invalid values, and partial channel configuration therefore produce
honest per-channel gaps without discarding other valid channels.

## Storage summary

- Directories: `/history/v1/real/` and `/history/v1/demo/`
- Active/interrupted name: `h1-r-s0000000123-m0000000000.open` (or `h1-d-...`)
- Closed name: `h1-r-s0000000123-m0000000000-n0240.bin`
- Row: 56 bytes containing energy, per-channel/component coverage, configured
  mask, and minute quality flags
- Flash cadence: five rows / 280 bytes / approximately five minutes
- Rotation: 240 rows / approximately four hours and every new boot/session
- Retention: maximum 200 files independently per dataset
- Anchors: bounded V1 ledger keyed by nonzero boot session, plus fixed Demo-fixture metadata

A stale `.open` file remains graphable after reboot. Complete rows are retained
and torn trailing bytes are ignored. The zero-to-five rows still in RAM at
power loss are absent according to the current durability policy.

## Catalog, query, and reset

Settings -> Data lists only the selected dataset. The catalog is derived
from strict filenames, sizes, anchor metadata, and current RAM; it does not scan
row bodies. New files update the catalog incrementally and appear without a
reboot.

The browser catalog/query API follows the active sensor source. It has no
Real/Demo parameter. Queries seek directly to required rows and return time
coverage plus per-channel/component measurement coverage.

Reset is dataset-scoped. Real reset cannot touch Demo; Demo reset cannot touch
Real. Factory/development reset may clear both and then recreate only protected
Demo fixtures after explicit confirmation.

## Verification status

This V1 candidate is implemented and builds. It must still pass
[HISTORY_TEST_PLAN.md](HISTORY_TEST_PLAN.md), including tenant isolation,
partial coverage, interruption recovery, natural rotation, retention, and
multi-day accuracy, before it is production durable.
