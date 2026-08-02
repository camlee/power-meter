# History Storage V1 candidate

## Status and migration boundary

This is the candidate production contract that will replace the current alpha
history implementation informally called V3. The names are being reset because
no production data exists: this format begins at V1 and no backward-compatible
reader or migration is required.

The first candidate firmware may delete `/history/v3/` and its alpha anchor
ledger. The single development device can be wiped before verification. Future
changes to candidate V1 require an explicit compatibility decision rather than
reusing the same format identity silently.

Goals:

- continue recording without wall time or networking;
- retain complete records from interrupted sessions;
- represent configured, valid, partial, and missing channels honestly;
- keep Real and Demo data isolated without duplicating the storage engine;
- bound boot, query, directory, and retention work;
- support deterministic fixtures and production history through the same parser
  and query code.

## Datasets: Real and Demo

History has two logical datasets, implemented as tenants of one format:

| Dataset | Recorded sources | Purpose |
| --- | --- | --- |
| `Real` (`r`) | ESP32 ADC and UART | Actual device observations. |
| `Demo` (`d`) | Realtime Demo source plus protected fixture files | UI development, deterministic history, and demonstrations. |

Routing is automatic from source provenance. A user cannot accidentally direct
ADC/UART samples into Demo or simulated samples into Real. Sensor source mode
and history view are independent:

- selecting a source determines where new records are written;
- Settings -> Data `Real | Demo` filters only the diagnostic file catalog;
- the filter does not install, remove, copy, or reclassify files;
- the filter defaults to the current sensor source whenever the page is entered
  and is reset when the page is left;
- choosing the non-active dataset is therefore a deliberate, page-local
  diagnostic action;
- Usage and browser APIs resolve the dataset from the active sensor source and
  do not expose a Real/Demo request parameter.

A Demo source continues recording into Demo even while the user inspects the
Real file catalog, and those files appear as soon as Demo is selected again.

## Directories and filenames

Candidate V1 uses separate bounded directories plus an explicit dataset marker
in every filename:

```text
/history/v1/real/h1-r-s0000000123-m0000000000.open
/history/v1/real/h1-r-s0000000123-m0000000000-n0240.bin

/history/v1/demo/h1-d-s0000000124-m0000000000.open
/history/v1/demo/h1-d-s0000000124-m0000000000-n0240.bin
```

Fields:

- `h1`: candidate history format V1;
- `r` / `d`: Real or Demo dataset;
- `s`: boot session ID;
- `m`: first complete monotonic minute in that session;
- `n`: number of complete records in a normally closed file;
- `.open`: active for the current matching session/dataset, otherwise evidence
  of interruption;
- `.bin`: cleanly closed after normal rotation.

The directory and filename dataset must agree. Unknown names, mismatches, and
malformed fields are ignored by queries and reported by diagnostics.

## Session and segment identity

The time service increments one persistent nonzero `uint32_t` boot session ID.
A normal boot records to only the dataset implied by its selected source, so
session IDs remain globally ordered without tenant-specific counters.

One session may contain several segments. A segment closes after 240 complete
minute rows (approximately four hours). A reboot starts a new session and a new
logical segment. Files are created on the first flash batch rather than at boot,
avoiding empty files from short sessions.

Session ID zero is reserved for immutable Demo fixture files and is never used
for recorded samples.

## Minute record

Each row represents one complete monotonic minute and carries energy plus valid
coverage. Filename position supplies session-monotonic time.

```cpp
struct __attribute__((packed)) MinuteEnergyRecordV1 {
    float channelEnergyWh[3];
    float componentEnergyWh[5];
    uint16_t channelCoverageMs[3];
    uint16_t componentCoverageMs[5];
    uint8_t configuredChannelMask;
    uint8_t qualityFlags;
    uint16_t reserved16;
    uint32_t reserved32;
};
static_assert(sizeof(MinuteEnergyRecordV1) == 56);
```

Coverage values are in the inclusive range 0–60,000 ms. They make these cases
distinct:

- configured channel with measured zero power: coverage is present, energy is
  zero;
- configured channel valid for part of a minute: partial coverage and its
  integrated energy are retained;
- configured but missing/invalid channel: zero coverage;
- channel intentionally absent: its configured-mask bit is clear.

Components retain their own coverage because net battery and duty-derived
metrics can require more inputs than a single channel.

Initial `qualityFlags`:

- bit 0: at least one configured observation was rejected as invalid or out of
  calculation range;
- bit 1: at least one configured source/channel was stale or missing;
- remaining bits are zero and reserved.

Reserved fields must be written as zero and ignored when reading candidate V1.
The ESP32 IEEE-754 little-endian float representation is part of the local file
contract.

Consequences:

- 240 rows = 13,440 bytes per normal segment;
- five rows = one 280-byte append approximately every five minutes;
- committed `.open` rows = `file_size / 56`;
- trailing `file_size % 56` bytes are an incomplete final write and ignored;
- a `.bin` count must agree with both `n####` and file size.

No row checksum is initially required. Count/size validation detects torn rows,
not silent storage corruption; field evidence can justify a future format.

## Energy integration and gaps

The sensor service supplies eligible channel/component values and interval
coverage according to [SENSOR_DATA_POLICY.md](SENSOR_DATA_POLICY.md). The
history accumulator:

- integrates only intervals bounded by eligible samples;
- never carries the last good sample through startup, timeout, or invalid data;
- maintains channel and component energy/coverage independently;
- emits one row for each completed monotonic minute in an active recording
  session, even when some configured channels have no coverage;
- exposes missing coverage in queries rather than substituting zero.

A whole-device outage creates no rows. A source with an effective configured
mask of zero also creates no rows or empty files. Once at least one channel is
configured, a live source/channel interruption can create rows with partial or
zero coverage, allowing the timeline to distinguish device uptime from sensor
availability.

## Interrupted files and flash buffering

Five completed rows are buffered in PSRAM and appended as one bounded write.
On reboot, stale `.open` files remain valid history:

1. every complete 56-byte row is readable;
2. an incomplete trailing row is ignored;
3. the `.open` name remains to preserve interruption evidence;
4. unwritten RAM rows are honest missing data;
5. older committed files are never rewritten during recovery.

The accepted durability policy therefore loses zero to five buffered minutes
on sudden power loss. Verification will decide whether production should reduce
that interval.

## Time anchors

Wall-clock mapping remains separate from measurement rows and datasets. The
bounded atomic ledger is stored under `/history/v1/` and keys ordinary anchors
by globally unique nonzero boot session ID. Each entry retains session monotonic
instant, Unix UTC milliseconds, fixed UTC offset, source, and uncertainty.

For ordinary recorded Real and Demo sessions, anchors follow the same rules:

- retain the first useful anchor;
- replace it only with materially better source/uncertainty;
- do not persist routine repeated NTP callbacks;
- infer an unanchored session block only when surrounding anchored sessions
  bound its uncertainty within the selected query bucket;
- compact an anchor only when neither dataset retains a referenced file.

Before the current boot has an anchor, rolling queries aggregate only its
matching session ID exactly in the monotonic-minute domain. Usage rolling
queries may additionally place unresolved prior sessions with an explicit
one-minute assumed gap between retained session extents. This placement exists
only in query scratch memory, carries a distinct assumed-time flag, and is
discarded as soon as direct or bounded-inferred placement is available. It is
never persisted or used by calendar and cycle queries. Fixture history remains
absent until its fixed anchor can be established. Calendar queries continue to
require wall time.

Time is a device property, not a history-view property: NTP/browser contribution
never depends on whether Real or Demo is being viewed. Reserved fixture session
zero uses separate fixed Demo-fixture anchor metadata and never participates in
ordinary boot-session inference.

Full timezone/DST rules remain outside candidate V1; persisted fixed UTC offset
defines local calendar boundaries.

## Protected Demo fixtures

The firmware contains the compact profile needed to generate a bounded fixture
set; application images do not need to carry a LittleFS filesystem image.
Storage initialization idempotently ensures the expected fixture version under
the Demo dataset and never changes Real files.

Fixture policy:

- filenames use Demo dataset and reserved session zero;
- fixture rows use the exact candidate V1 parser and query path;
- fixtures are protected from ordinary Demo retention/reset;
- installing a new firmware does not rewrite an unchanged fixture version;
- changing the fixture version replaces session-zero fixture files only;
- recorded Demo sessions use normal nonzero boot IDs and coexist with fixtures.

Fixture V2 is deterministically generated by `tools/build_demo_profile.py` from
timestamped `demo-source` logs 0 through 29. It covers 29 hours 45 minutes as
119 fifteen-minute profile points and writes 1,725 measured minutes into eight
segment files. Missing `22.csv` becomes a one-hour gap between fixture segments;
the generator never interpolates it or converts it to zero.

Fixtures must not slide forward on every boot because that could overlap and
double-count recorded Demo sessions. Once trustworthy wall time is available,
the fixture anchor is pinned once with its start approximately 48 hours before
current time. If that window overlaps a resolved recorded Demo interval, it is
moved earlier to a complete local-day boundary. It then remains fixed.

If recorded Demo sessions exist but cannot yet be placed defensibly, fixture
anchoring waits rather than guessing. Queries must never sum fixture and
recorded Demo coverage for the same interval.

If the device has no wall time, fixture files may exist but remain absent from
calendar queries until their one-time non-overlapping anchor can be created. An explicit
`Reset Demo` action removes recorded Demo sessions and regenerates/re-anchors
the protected fixture set; it must disclose that behavior before confirmation.

## Catalogs, query, and API

The storage engine scopes every operation to exactly one dataset. Runtime Usage
and browser services derive that dataset from the active sensor source; the
Settings -> Data file-catalog request may instead use its transient page-local
filter. Only the resulting directory is cataloged, keeping normal work bounded
even though both datasets coexist.
Catalog entries come from strict filenames, file sizes, the anchor ledger, and
current RAM state; building a catalog never scans record bodies.

Rolling and calendar queries:

1. select exactly one dataset;
2. resolve candidate file intervals from anchors and bounded inference;
3. discard non-overlapping files;
4. seek directly to required fixed-size rows;
5. aggregate energy plus per-channel/component coverage;
6. propagate time inference separately from measurement coverage;
7. show gaps when configured coverage is materially missing.

Query results expose timeline coverage and per-channel/component coverage. A
device with only `In` configured does not mark `Out` as zero or as a sensor
failure; net-battery metrics are simply unavailable.

The browser catalog/query API has no dataset selector. It always reads the
dataset selected by the active sensor source, matching the touchscreen Usage
view. This is a new V1 public contract; alpha compatibility is not required.

## Retention and reset

Each dataset has its own 200-file cap so Demo activity can never evict Real
history. With 56-byte rows, 200 full segments use about 2.7 MB per dataset; both
caps plus filesystem overhead fit within the approximately 9.9 MB data
partition. Only the selected dataset catalog is held in scratch memory.

- Real retention deletes the oldest non-active Real file only.
- Demo retention counts protected fixture files within its 200-file bound but
  never selects them as victims; the oldest recorded Demo file is deleted.
- The active file is never deleted.
- `Reset Real` clears Real files and newly orphaned anchors only.
- `Reset Demo` clears recorded Demo files and restores the protected fixtures.
- A development/factory `Reset All History` clears both datasets and anchors,
  then recreates only the protected Demo fixture set.

## Verification status

Candidate V1 is implemented and builds, but it must pass
[HISTORY_TEST_PLAN.md](HISTORY_TEST_PLAN.md) before being treated as production
durable. Per-dataset reset currently leaves harmless bounded orphan anchors;
safe cross-tenant anchor compaction remains a follow-up. Alpha V3 data requires
no migration and may be removed from the single development device.
