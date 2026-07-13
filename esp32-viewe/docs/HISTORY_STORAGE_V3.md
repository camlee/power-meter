# History Storage V3 Design

## Status and goals

This document is the agreed design direction for the next history-storage
implementation. It intentionally favors simple, bounded filesystem operations
over database-like indexing. V3 is a fresh development format; V1/V2 migration
is not required.

Goals:

- continue recording without wall time or networking;
- make boot, Settings diagnostics, and ordinary graph queries independent of a
  full measurement-data scan;
- retain complete records from interrupted sessions;
- bound directory growth to 200 measurement files;
- write flash in five-record batches;
- let the future web application use the same small JSON/file APIs as the local
  diagnostics UI.

## Session and segment identity

The time service persists a monotonically increasing `uint32_t` session ID in
NVS and increments it once per boot. Wraparound after approximately four billion
boots is outside the practical product lifetime but filenames and comparisons
must still use the full 32-bit value.

One session may contain several segments. A segment closes after 240 complete
minute records (approximately four hours), and a new segment begins in the same
session. A reboot always starts a new session and therefore a new logical
segment.

Recording is aligned to complete ESP monotonic minutes. A partial minute during
startup is not persisted. This keeps the mapping from filename plus record index
to session-monotonic time exact and bounds startup loss to less than one minute.

## Directory and filenames

V3 data belongs under `/history/v3/`. Fixed-width decimal fields preserve
lexicographic chronological order and remain human-readable.

Active or interrupted segment:

```text
h3-s0000000123-m0000000000.open
```

Normally finalized segment:

```text
h3-s0000000123-m0000000000-n0240.bin
```

Fields:

- `h3`: format generation;
- `s`: boot session ID;
- `m`: first complete monotonic minute in that session;
- `n`: number of records in a cleanly finalized file;
- `.open`: active when its session matches the running session, otherwise
  interrupted by reset/power loss;
- `.bin`: cleanly closed after a normal 240-record rotation.

The active file is the `.open` file whose session ID matches the current boot.
There may be many stale `.open` files from prior interrupted sessions. They are
valid history files, not temporary files to discard.

Files should be created on the first actual flash batch rather than immediately
at boot, avoiding empty files from sub-minute/short reboot sessions. Until then,
the History UI reports the current segment from RAM only.

All filenames are parsed strictly. Unknown or malformed files are ignored by
history queries and reported separately by diagnostics rather than guessed.

## Measurement record

Each row represents exactly one complete monotonic minute. Its position supplies
its time; its filename supplies its session and first minute.

```cpp
struct __attribute__((packed)) MinuteEnergyRecordV3 {
    float channelEnergyWh[3];
    float componentEnergyWh[5];
};
static_assert(sizeof(MinuteEnergyRecordV3) == 32);
```

The firmware assumes the ESP32's IEEE-754 little-endian float representation.
V3 files contain only these records: no header and no footer.

Consequences:

- 240 records = 7,680 bytes per normal four-hour segment;
- five buffered records = one 160-byte append every five minutes;
- committed count in `.open` = `file_size / 32`;
- a trailing `file_size % 32` is an incomplete final write and is ignored;
- clean `.bin` record count must agree with both `n####` and `file_size / 32`.

No per-record checksum is planned initially. File-size/count validation detects
torn rows but not silent bit corruption. A checksum can be reconsidered only if
field evidence justifies its runtime/format cost.

## Interrupted `.open` files

A reset after three hours commonly leaves a stale `.open` file. On the next
boot, the firmware:

1. recognizes it as stale because its session ID differs from the current one;
2. uses every complete 32-byte record;
3. ignores any incomplete trailing bytes;
4. leaves the `.open` name intact so diagnostics preserve the interruption fact;
5. includes its records in rolling graphs and, when its session is anchored or
   inferable, calendar graphs.

The last zero-to-five minutes still in RAM at power loss are absent. That is an
honest timeline gap. The existing rule applies: missing coverage must exceed one
minute before the UI shows its small warning symbol.

## Time anchors

Wall time is separate from measurement segments:

```text
/history/v3/time-anchors.bin
```

The ledger holds at most one useful mapping per session:

```cpp
struct TimeAnchorV3 {
    uint32_t sessionId;
    uint64_t anchorMonotonicMs;
    int64_t unixTimeMs;
    int16_t utcOffsetMinutes;
    uint8_t source;
    uint32_t uncertaintyMs;
};
```

The final packed representation may add a format byte and checksum, but should
remain fixed-size. The entire ledger is intentionally small enough to read into
RAM at boot.

Anchor update policy:

- add the first valid anchor for a session;
- ignore repeated equal-or-worse anchors for that session;
- replace the retained entry only when the new source/uncertainty is materially
  better (for example NTP replacing browser time);
- do not persist routine repeated NTP callbacks;
- update the small ledger atomically through a temporary file and rename rather
  than appending useless duplicates;
- occasionally compact away sessions for which no measurement file remains.

Source quality and numeric uncertainty both participate in comparison. A large
conflict from a nominally equal source is diagnostic evidence, not a reason to
accumulate every sample.

For an anchored session:

```text
recordMonotonicMs = (fileFirstMinute + recordIndex) * 60,000
recordUnixMs = anchorUnixMs + recordMonotonicMs - anchorMonotonicMs
```

The fixed UTC offset used for calendar boundaries remains a clock policy; the
anchor offset records provenance and aids diagnostics.

### Bounded inference

An unanchored session or ordered block of sessions may enter any wall-clock
Usage view only when anchored data bounds both sides. The catalog supplies
recorded durations without reading row bodies. Its nonnegative unexplained time
is the block's timestamp uncertainty. A query accepts the block only when that
uncertainty is no larger than its displayed bucket width, so a daily graph can
honestly show more bounded data than a two-minute graph. Accepted downtime is
distributed evenly across reboot boundaries for a stable estimated placement and
the relevant buckets retain `TIME_INFERRED`; otherwise the graph shows a gap.

## File catalog

A catalog entry is derived only from filename parsing, filesystem-reported size,
the small anchor ledger, and current RAM state. It contains approximately:

```text
name, session ID, first session minute, complete record count,
open/closed, active/interrupted, byte size,
anchored/inferred/unanchored, resolved start/end when available
```

No measurement rows are read to create this catalog. With a hard 200-file cap,
directory enumeration and sorting are bounded. The implementation builds the
catalog once in PSRAM at boot, then updates it incrementally after a batch write,
rename, retention deletion, or reset. UI/API reads therefore do not repeatedly
walk LittleFS, and the bounded cache does not occupy scarce internal RAM.

For a closed file, count comes from `n` and is validated against size. For a
stale `.open`, count comes from size. For the current active segment, the catalog
adds direct RAM information: pending records, current duration, active anchor
state, and committed-plus-buffered size.

## Graph query algorithm

### Last-hour ranges

For Last 1/6/24 Hours, compute the real wall-clock interval ending at now and
apply the same anchored/bounded-inference resolution as calendar ranges.
Unresolved data stays empty rather than being right-aligned to now. Stale
`.open` files participate normally and current RAM records follow the active
file's committed records.

### Calendar ranges

For Today, Yesterday, Last 2 Days, Last Week, Last Two Weeks, and All:

1. compute the requested Unix interval from the current fixed UTC offset;
2. read the bounded in-memory file catalog;
3. resolve each anchored/inferred file's Unix start/end using its session anchor;
4. discard catalog entries that do not overlap the requested interval;
5. for each overlapping file, calculate the first and last required record index
   arithmetically from its resolved start time;
6. seek directly to `recordIndex * 32` and sequentially read only the overlapping
   records;
7. split a minute's energy proportionally only when it crosses a graph/calendar
   boundary;
8. merge current RAM records when the active segment overlaps;
9. leave uncovered buckets empty and set incomplete only when missing coverage
exceeds one minute; inferred coverage is surfaced separately to the UI.

Example: Yesterday normally selects about six four-hour files. It does not read
the other directory entries' bodies and does not scan selected files before the
needed row; filenames, sizes, and anchors already provide the offsets.

## Diagnostics UI and JSON API

Settings -> History lists files newest first, with the session visually
prominent. The on-device list should be bounded to roughly 20-25 entries and
show:

- total recorded days/hours/minutes, file count/cap, and storage in MB;
- session and recorded duration for each file;
- a check mark for cleanly closed files;
- resolved local begin/end when the file's session has a direct anchor.

The same catalog is exposed through a paginated authenticated endpoint:

```http
GET /api/v1/history/files?offset=0&limit=25
```

The server caps `limit`; it must not construct a 200-file JSON document in RAM.
A later explicit endpoint can inspect/download one file's rows:

```http
GET /api/v1/history/files/{name}
```

Filename input must be parsed/validated rather than accepted as an arbitrary
filesystem path.

## Retention

Measurement history is capped at 200 files, including stale `.open` files but
excluding `time-anchors.bin` and temporary metadata files. Before creating a new
segment, delete the oldest non-active measurement files until fewer than 200
remain. Never delete the current active segment.

At six normal segments per day, 200 files is approximately 33 days. Fourteen
days uses 84 files, leaving room for reboot-created small segments while still
placing a hard bound on directory growth. Anchor-ledger compaction follows file
retention so orphaned mappings do not grow indefinitely.

## Recovery and reset

- Validate `.bin` count against size and report mismatches.
- Ignore incomplete trailing bytes in `.open`; an explicit maintenance operation
  may truncate them later.
- A failure to rename a full `.open` file must not overwrite it; start no
  conflicting filename.
- Usage Data reset deletes recorded V3 measurement segments and the anchor ledger,
  temporary ledger files, and pending RAM records, then restarts cleanly.
- V1/V2 files are development artifacts and can be removed by reset rather than
  migrated.

## Implementation and verification status

Implemented: strict filenames, 32-byte rows, five-row appends, 240-row rotation,
stale `.open` reads, 200-file retention, the deduplicated anchor ledger, bounded
inference, catalog-driven rolling/calendar queries, Settings -> History, Usage
reset, and the paginated JSON catalog.

Build, USB flash, complete UI construction, an actual five-row/160-byte append,
catalog pagination (including 50 files), and stable device uptime have been
verified. The destructive and
long-running cases remain explicit acceptance work: torn writes, repeated boot
recovery, actual 240-row rotation, 200-file eviction, bounded inference across
crafted sessions, and multi-day totals.
