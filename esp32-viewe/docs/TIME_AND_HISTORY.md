# Time and History V3

This is the behavioral overview for the implemented time/history service. The
exact storage contract and algorithms are specified in
[`HISTORY_STORAGE_V3.md`](HISTORY_STORAGE_V3.md).

## Timeline behavior

Every boot receives a persistent monotonic `uint32_t` session ID. Measurement
order is available without Wi-Fi or wall time: each segment filename contains
its session and first complete monotonic minute, and every following 32-byte row
represents the next minute.

NTP and the browser time endpoint submit the same anchor relationship:

- session ID and session-local monotonic instant;
- Unix UTC milliseconds;
- fixed local UTC offset;
- source and declared uncertainty.

The bounded LittleFS anchor ledger keeps one useful anchor per session. Repeated
clock refreshes are accepted for the live clock but do not rewrite the ledger
unless their precision is materially better. A wholly unanchored block is
placed only when anchored data surrounds it. Its total unexplained downtime is
timestamp uncertainty, and it may be shown only when that uncertainty is no
larger than the selected graph's bucket width.

All Usage ranges use the same real-time timeline, including Last 1/6/24 Hours.
They leave unresolved/missing intervals empty rather than moving record-order
data to "now." Calendar views use a persisted fixed UTC offset and provide
Today, Yesterday, Last 2 Days, Last Week, Last Two Weeks, and All. Today and
Last 2 Days retain their complete local-day axes after the current time.
Missing coverage must exceed one minute before the Usage screen shows its small
warning symbol; inferred coverage instead adds "some timestamps inferred."

## Browser contract

The future same-origin web app should submit time when it connects:

```http
POST /api/v1/time/anchor
Authorization: Bearer <device token>
Content-Type: application/json

{"unix_ms":1783890123456,"utc_offset_minutes":-360}
```

In JavaScript, use `Date.now()` and
`-new Date().getTimezoneOffset()`. The offset is optional and must be within
plus/minus 14 hours. The current model deliberately stores a fixed offset, not
IANA timezone or DST rules.

## Storage summary

- Directory: `/history/v3/`
- Active/interrupted name: `h3-s0000000123-m0000000000.open`
- Closed name: `h3-s0000000123-m0000000000-n0240.bin`
- Row: eight floats / 32 bytes (three channel energies and five component energies)
- Flash cadence: five rows / 160 bytes / approximately five minutes
- Rotation: 240 rows / approximately four hours, and every new boot/session
- Retention: maximum 200 measurement files
- Anchors: `/history/v3/time-anchors.bin`

A stale `.open` file remains graphable after reboot. Its complete row count is
`floor(fileSize / 32)`; any torn trailing bytes are ignored. Closed-file counts
are obtained from the filename and checked against size. The current active
file additionally exposes pending PSRAM rows.

In Demo mode the device seeds ten four-hour synthetic segments across three local days,
marks one historical day inferred. This keeps Today, Last Two Weeks, and All
visually useful on a new device and exercises the small inference disclosure.

The authenticated catalog endpoint reads filenames and sizes, not row bodies:

```http
GET /api/v1/history/files?offset=0&limit=25
```

Settings -> History uses the same bounded catalog and lists files newest first.
Its summary reports recorded minutes as days/hours/minutes, file count/cap, and
storage in MB. Each file is a compact two-line session/duration entry; cleanly
closed files have a check mark and directly anchored files include local start
and end times.
Usage Data reset removes recorded V3 segments, anchor-ledger files, and pending rows;
the synthetic Demo segments are retained.
Development V1/V2 data is not migrated.

## Query behavior

Usage queries resolve catalog intervals first, discard non-overlapping files,
calculate row offsets arithmetically, and seek directly to the required 32-byte
records. Direct anchors use exact positions. For an accepted bounded block,
spare time is distributed evenly across its reboot boundaries for a stable
estimate while remaining explicit uncertainty. Energy is split proportionally
only across a query boundary.

## Verified and remaining

Verified on the attached ESP32-S3:

- clean build and USB flash;
- stable construction of the complete Settings UI;
- authenticated info and history catalog requests, including a 50-file page;
- file-oriented History and memory Debug screens;
- an actual five-row/160-byte flush into the active session file;
- stable uptime after the memory corrections.

Still requiring longer-running/destructive tests:

- power-loss recovery during/around a five-minute append;
- stale `.open` behavior across repeated boots;
- 240-row close/rename and 200-file retention deletion;
- bounded inference across intentionally anchored/unanchored sessions;
- multi-day calendar totals against independently known energy.
