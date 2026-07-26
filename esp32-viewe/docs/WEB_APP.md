# Embedded Web App

## Milestone status and scope

The embedded web-application milestone is complete. The normal firmware image
contains a Svelte 5 single-page application with live power, per-channel sensor
observations, browser time contribution, rolling/calendar history, daily
energy-cycle balance, device information and diagnostics, device setup,
independent Web appearance, and capability-gated remote-display control. It is a useful product surface, not a requirement to
duplicate every LVGL page.

The browser and on-device Cycle views are complete for the current product
scope and consume the same recent-cycle summary model.

Calibration, storage browsing/export, and further appearance refinements
remain incremental backlog to build as needed.

The old MicroPython project is a behavioral reference only. Its Parcel,
Material UI, Chart.js, CSV parsing, and text/CSV websocket protocol are not
carried forward.

## Local build and verification

Node.js 20+ is required once per development machine:

```sh
cd esp32-viewe/web
npm ci
npm run check
```

`npm run check` builds the production SPA and verifies that the generated
firmware asset table is current. The same build runs automatically as a
PlatformIO pre-build action, so these normal commands always contain the web
app:

```sh
pio run -d esp32-viewe -e viewe
pio run -d esp32-viewe -e wroom
```

There is intentionally no CI/CD system for this project. The local checks are
the release gate: a web build fails if its gzip payload exceeds 256 KiB; the
normal PlatformIO size check must leave room in the selected target's OTA
application slot (3 MiB on VIEWE, 1.5 MiB on WROOM).

`tools/build_web_assets.py` runs Vite, gzip-compresses each emitted asset with
a reproducible timestamp, and generates ignored C++ source in
`src/network/web_assets.generated.*`. Those byte arrays are flash-resident and
part of `firmware.bin`; LittleFS remains for meter history. Consequently the
existing signed application OTA updates the UI atomically with its compatible
firmware/API. No filesystem image upload is required.

## Finding a physical meter

The serial monitor reports the current endpoint at boot and whenever Wi-Fi or
the AP changes. Its stable form is deliberately easy for a person or script to
recognize:

```text
VIEWE_NETWORK state=4 station=192.168.1.217 ap=192.168.4.1 ap_ssid=meter4j host=meter1.local
VIEWE_WEB url=http://192.168.1.217/ host=meter1.local
```

The VIEWE board mirrors these through both Arduino `Serial` and ESP-IDF logging,
so they appear on its USB JTAG serial console even though USB CDC is disabled
in the current board configuration.

The firmware also advertises `_viewe-ota._tcp.local` over mDNS. Run the local,
dependency-free helper from the project root to find it without probing an
entire subnet:

```sh
python3 tools/discover_device.py
python3 tools/discover_device.py --host 192.168.1.217
```

`--host` is the deterministic fallback when the serial monitor has supplied an
IP or the LAN blocks multicast. The helper uses `avahi-browse` when present and
then its dependency-free direct mDNS resolver for the configured
`meter1.local` fallback; change `--hostname` for a renamed meter. Resolve known
names directly with `python3 tools/mdns_resolver.py meter1 sensor1`.

## Serving and browser cache policy

The Arduino HTTP service on port 80 serves the generated gzip bytes directly
from program flash. It does not mount, enumerate, or allocate a LittleFS file
for a static request.

| Resource | Response policy |
| --- | --- |
| `index.html` / SPA routes | `no-cache, max-age=0, must-revalidate`, ETag |
| Vite-hashed JS/CSS | `public, max-age=31536000, immutable` |
| JSON/status, display capture, API writes | `no-store` |

The tiny `index.html` includes its own CSS and indeterminate progress element.
It is visible while the browser downloads the hashed JavaScript/CSS chunks,
including on a first load over the meter's AP. There is no service worker: the
HTML shell is always revalidated after an OTA, then it points to new hashed
assets while already cached assets incur no transfer.

## HTTP and realtime protocol V1

The first public read model is deliberately small:

```text
GET  /api/v1/web/status       JSON, no-store
GET  /api/v1/sensors          raw sensor/source diagnostics, JSON, no-store
GET  /api/v1/setup            persisted device setup, JSON, no-store
POST /api/v1/setup            validate, persist, and restart
GET  /api/v1/wifi             station/AP state, scans, saved networks and AP clients
POST /api/v1/wifi/station     scan/connect/disconnect/connect-saved/forget commands
POST /api/v1/wifi/ap          validate and apply access-point settings
GET  /api/v1/debug            on-device Debug read model, JSON, no-store
POST /api/v1/time/anchor      local-LAN browser time anchor
GET  /api/v1/display/...      local-LAN remote display control (`meter-viewe` only)
WS   ws://<meter>:81/api/v1/live
GET  /api/v1/history/query?range=today&bucket_minutes=30
GET  /api/v1/history/query?job=<id>
GET  /api/v1/cycles
GET  /api/v1/cycles?job=<id>
POST /api/v1/cycles              {"end_hour":20}
```

The WebSocket is on port 81 only for this first compatibility slice: port 80
continues to use the established synchronous Arduino `WebServer` for the
authenticated signed OTA workflow. The realtime listener is ESP-IDF's native
HTTP server, has five client slots, and only publishes an 84-byte frame at 2 Hz.
The SPA computes that port from the host name, so the browser user never enters
it. A later migration of OTA routes to the native server can unite both on port
80 without changing the browser protocol path or frame layout.

The live WebSocket, sensor read model, browser time anchor, and remote-display
endpoints are unauthenticated for use on a trusted local network. The Wi-Fi
settings endpoints follow the same local-management model and the Wi-Fi read
model includes the saved AP password so it can reproduce the on-device editor.
OTA and its
maintenance diagnostics retain their bearer-token policy. Do not expose the
meter beyond the trusted LAN without adding authentication to these
browser-facing endpoints.

The Wi-Fi settings page uses the same `network_manager` command and persistence
methods as the LVGL Wi-Fi screen. Station scans and connections are asynchronous;
the page polls the small Wi-Fi read model once per second while visible to show
scan, connection, retry, and credential-failure state. A user can select a scan
result or enter a hidden SSID, connect with new credentials, reconnect to or
forget a saved network, and explicitly disconnect station mode. Access-point
settings include enabled/disabled, SSID, open/secured mode, password, IP address,
and connected client MAC addresses. Applying a different active AP configuration
can disconnect the browser, which must then join the new SSID.

`/api/v1/web/status` publishes the hardware profile and capabilities, including
touch/status displays and individually supported sensor modes.
The SPA hides remote-display, LVGL appearance, and unsupported source controls
at runtime, allowing the same embedded assets to serve both hardware targets.

`/api/v1/sensors` is the diagnostic and calibration read model used by the
Sensors and Setup pages. Each channel reports `configured`, `observed`, its
`valid | out_of_range | not_configured | waiting | invalid | stale` state,
sample age, finite voltage/current/power observations, and optional duty. A
finite out-of-range observation remains visible here; unavailable values are
`null`. Operational Power and History interfaces remain calculation-eligible
only. UART mode additionally exposes receiving/waiting/stale connection state,
last-valid age, advertised channel mask, sequence and producer uptime, parser
error, and bounded frame/error counters. ADS1115 mode distinguishes initialization
from recent conversion health and reports success age, consecutive failures,
bus errors, and lock timeouts. Physical ADC modes also expose raw input volts
and the active source's gain/offset/defaults. Calibration writes use
`POST /api/v1/sensors/calibration`; unwired channels and non-calibrated sources
are rejected. Demo reports `transport: null` rather than inventing connection
semantics.

Physical ADC modes also expose the shared on-demand acquisition capture:

- `POST /api/v1/sensors/capture` with `{"channel":"in"}` (or `out`/`aux`)
  arms capture at the next 500 ms reducer boundary and returns `capture_id`;
- `GET /api/v1/sensors/capture` reports the bounded
  `idle | armed | capturing | ready` lifecycle and current ID;
- `GET /api/v1/sensors/capture/data?id=<capture_id>` transfers and releases the
  completed versioned binary result;
- `DELETE /api/v1/sensors/capture?id=<capture_id>` cancels that exact request.

Only one capture can be active or awaiting transfer. It contains the selected
channel's exact calibrated V/A/W observations and three corresponding
production reducer windows; it does not initiate separate ADC reads. The
self-describing header includes requested/measured intervals, point/window
sizes, duration, and dropped-point count. Each window also carries its
configured/reading/duty state, so a rejected finite observation is never
presented as eligible production data. Active requests expire after 10 seconds
and unclaimed ready results after 30 seconds. Generation-checked take/cancel
operations prevent a stale display or browser from affecting a newer capture.
`web/src/lib/api.js` is the normative binary parser. The Sensors page starts
this flow with **View Raw**, switches to a focused capture view, and offers an
explicit repeat capture.

The V4 binary frame is exactly 84 bytes, packed and little-endian; do not map a
future C++ struct directly in browser code. Each new connection receives the
most recent 60 frames (about 30 seconds) in chronological order before normal
2 Hz delivery resumes. `web/src/lib/api.js` parses it with `DataView` and is
the normative current browser implementation. The boot/session ID increments
on every firmware boot and remains constant across WebSocket reconnects. The
browser pairs it with the frame sequence to reject replayed frames without
mistaking an ordinary reconnect for a reboot.

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `u32` | magic `VPM4` (`0x344d5056`) |
| 4 | `u8,u8,u16` | version `4`, type `1`; flags: bit 0 wall time, bits 1–3 configured, bits 4–6 eligible, bits 7–9 observed |
| 8 | `u32` | increasing frame sequence |
| 12 | `u32` | device state revision |
| 16 | `u32` | uptime milliseconds |
| 20 | `u32` | boot/session ID |
| 24 | `f64` | Unix milliseconds, or NaN before anchoring |
| 32 | `f32 × 10` | In V/A/W, Out V/A/W, Aux V/A/W, net battery W |
| 72 | `f32 × 3` | In, Out, and Aux duty; NaN when unavailable |

The browser retains VPM2 and VPM3 parsing during the development transition,
but new firmware publishes VPM4. Sensors charts retain the replayed 30-second window,
draw gaps for unavailable observations, and can overlay a staged calibration
preview without changing device state until Save succeeds.

Cycle queries use the same asynchronous history worker pattern: the first GET
returns a job ID and polling returns the bounded recent window through the
current cycle. The persisted
`end_hour` is shared by LVGL and Web, is validated from 0 through 23, and can
be changed without restarting. Each summary contains charged, used, and net Wh,
coverage, quality, current/incomplete flags, and UTC interval bounds. Charge is
solar input adjusted by the configured model efficiency. The Web Cycle table
lists the most recent cycle first. Coverage that is too sparse is exposed as
unavailable; usable partial coverage remains visible with an incomplete-data
warning.

Cycle calendar boundaries intentionally use the device's current persisted
fixed UTC offset. They remain fixed 24-hour intervals and do not attempt to
reconstruct historic daylight-saving transitions; see `ARCHITECTURE.md`.

History uses a separate magic/type and an explicitly documented compact bucket
schema. The two-step asynchronous query starts a bounded history worker job,
polls until ready, then receives a binary VPH2 response (32-byte header
followed by 80-byte buckets). `range` supports the calendar ranges `today`,
`yesterday`, `last2days`, `lastweek`, `lasttwoweeks`, and `all`, plus the
rolling `last1hour`, `last6hours`, and `last24hours` ranges. The firmware, not
the browser, performs anchoring, gap, and calendar calculations. The browser turns
the compact Wh fields into average watts to use the same stacked chart semantics
and colours as the LVGL Usage screen.

VPH2 replaces the incompatible alpha VPH1 layout. Its 80-byte record keeps the
energy fields together and adds configured/time/quality flags plus independent
channel and component coverage:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `f64` | Bucket start Unix milliseconds |
| 8 | `u32` | Timeline coverage milliseconds |
| 12 | `u8 × 4` | Configured mask, time flags, quality flags, reserved zero |
| 16 | `f32 × 3` | In, Out, Aux energy Wh |
| 28 | `f32 × 5` | Component energy Wh |
| 48 | `u32 × 3` | Per-channel valid coverage milliseconds |
| 60 | `u32 × 5` | Per-component valid coverage milliseconds |

History always follows the active sensor source and exposes no Real/Demo
request parameter. Storage-format V1 and browser-protocol VPH2 are separate
contracts.

## Keeping web and LVGL state synchronized

Both surfaces must read/write the same services; neither owns a separate
settings model. A successful mutation persists through the relevant service
(Preferences/Time/History/etc.), updates the LVGL view through its normal
screen refresh, and calls `device_state::changed(domain)`.

`device_state` is a cheap monotonic invalidation token. It is currently wired
to device-ID, calibration, network command, and browser-time mutations. Each
live WebSocket frame carries the revision. When a visible browser notices a
change it refetches the small `/api/v1/web/status` read model immediately;
independent 10-second status polling is the fallback when no live socket is
connected. This gives touchscreen-originated changes near-immediate browser
visibility without having HTTP callbacks touch LVGL.

The Setup page stages hostname, sensor source, device appearance, and reset
scopes in the browser. `POST /api/v1/setup` validates the complete request and
uses the same persistence/service methods as LVGL before restarting, because
those settings are initialized at boot. Web appearance is a separate,
browser-local Light/Dark/Auto/Device preference: `auto` follows the browser OS
preference, while `device` follows the effective LVGL theme reported by
`/api/v1/web/status` and continues tracking it. Device Auto selects dark from
19:00 through 06:59 local time. Because the persistent LVGL screens cannot yet
be recolored safely in place, a boundary transition persists the next
effective appearance and performs a controlled restart; the persisted value
prevents a clock-sync restart loop. Do not manipulate UI widgets from a
network task or mirror Preferences directly in JavaScript.

Wi-Fi changes do not require a restart. The browser stages only the fields in
the visible form, then calls the shared manager through the Wi-Fi API. The API
validates ESP32 limits (SSID 1–32 bytes, WPA password 8–63 bytes), JSON-escapes
arbitrary SSID/password characters, and reports an in-progress conflict rather
than starting a second radio operation.

While Usage is visible, both browser and LVGL refresh just after a monotonic
storage boundary at a cadence equal to one displayed x-axis bucket (2 minutes
for Last 1 Hour through 4 hours for Last Week). The firmware-selected All
bucket controls its cadence after the first result. Yesterday is complete and
manual-refresh only. Queries remain asynchronous and hidden views do no work.

## Resource limits

- Web assets: 256 KiB gzip build budget; current build is reported by the local
  asset generator.
- Static requests: read from flash; browser performs gzip decompression.
- Live WebSocket: five clients, 2 Hz, 84-byte frames; it retains 60 frames
  (5.04 KiB) for initial replay. A sixth client receives a `limit:5` text
  notice and is closed. A queued send drops the next frame instead of building
  an unbounded backlog.
- Remote viewer: uses full-resolution BMP snapshots. A 320×480 capture uses
  about 450 KiB of temporary PSRAM and is rate-limited to four captures/second.
- Debug telemetry reports the LVGL task's configured stack and minimum free
  bytes observed since boot on touchscreen targets; web-first targets report
  `lvgl_stack: null`.
- History remains an asynchronous, bounded job in `history_query_service`;
  do not add a handler that scans files synchronously on the network/LVGL path.

## Web backlog

1. Replace the shared history worker's newest-request-wins result slot with job
   states that distinguish pending, completed, and superseded work. Concurrent
   LVGL/browser queries must not leave a client polling a discarded job forever.
2. Investigate and reduce the cost of rebuilding Cycle summaries from raw
   minute history. Measure storage-lock hold time and sampling impact, then
   consider cached or incrementally maintained cycle aggregates.
3. Add explicit raw-file export UI.
4. Expand Setup with additional device metadata when it becomes useful.
5. Migrate port-80 OTA/static routes to ESP-IDF HTTPD when preserving the
   existing updater semantics has dedicated verification coverage.
