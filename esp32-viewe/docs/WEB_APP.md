# Embedded Web App

## Scope of the first vertical slice

The meter now embeds a small Svelte 5 single-page application in the normal
firmware image. It provides an overview with live In/Out readings and a canvas
graph, a browser time-anchor control, a today/hourly history graph, and the
existing remote-display control. It is deliberately a foundation rather than a
duplicate of every LVGL page; editable settings build on the contracts below.

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
pio run -d esp32-viewe
pio run -d esp32-viewe -t upload
```

There is intentionally no CI/CD system for this project. The local checks are
the release gate: a web build fails if its gzip payload exceeds 256 KiB; the
normal PlatformIO size check must leave room in the 3 MiB OTA application slot.

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
VIEWE_NETWORK state=4 station=192.168.1.217 ap=192.168.4.1 host=meter1.local
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
IP or the development machine does not resolve `.local` names. The helper uses
`avahi-browse` when present and then attempts the configured `meter1.local`
fallback; change `--hostname` for a renamed meter.

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
POST /api/v1/time/anchor      local-LAN browser time anchor
GET  /api/v1/display/...      local-LAN remote display control
WS   ws://<meter>:81/api/v1/live
GET  /api/v1/history/query?range=today&bucket_minutes=30
GET  /api/v1/history/query?job=<id>
```

The WebSocket is on port 81 only for this first compatibility slice: port 80
continues to use the established synchronous Arduino `WebServer` for the
authenticated signed OTA workflow. The realtime listener is ESP-IDF's native
HTTP server, has five client slots, and only publishes a 64-byte frame at 2 Hz.
The SPA computes that port from the host name, so the browser user never enters
it. A later migration of OTA routes to the native server can unite both on port
80 without changing the browser protocol path or frame layout.

The live WebSocket, browser time anchor, and remote-display endpoints are
unauthenticated for use on a trusted local network. OTA and diagnostic
endpoints retain their bearer-token policy. Do not expose the meter beyond the
trusted LAN without adding authentication to these browser-facing endpoints.

The binary frame is exactly 64 bytes, packed and little-endian; do not map a
future C++ struct directly in browser code. Each new connection receives the
most recent 60 frames (about 30 seconds) in chronological order before normal
2 Hz delivery resumes. `web/src/lib/api.js` parses it with `DataView` and is
the normative current browser implementation.

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `u32` | magic `VPM1` (`0x314d5056`) |
| 4 | `u8,u8,u16` | version `1`, type `1`, flags (`bit 0`: wall time valid) |
| 8 | `u32` | increasing frame sequence |
| 12 | `u32` | device state revision |
| 16 | `u32,u32` | uptime milliseconds, reserved |
| 24 | `f64` | Unix milliseconds, or NaN before anchoring |
| 32 | `f32 × 8` | In V/A/W, Out V/A/W, Aux W, net battery W |

History uses a separate magic/type and an explicitly documented compact bucket
schema. The two-step asynchronous query starts a bounded history worker job,
polls until ready, then receives a binary VPH1 response (32-byte header
followed by 48-byte buckets). `range` supports the calendar ranges `today`,
`yesterday`, `last2days`, `lastweek`, `lasttwoweeks`, and `all`, plus the
rolling `last1hour`, `last6hours`, and `last24hours` ranges. The firmware, not
the browser, performs anchoring, gap, and calendar calculations. The browser turns
the compact Wh fields into average watts to use the same stacked chart semantics
and colours as the LVGL Usage screen.

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

Future browser settings writes must use the same service mutation methods as
LVGL and call the same `device_state::changed` point only after persistence and
readback succeed. A browser write should optimistically show a pending state,
then refresh status after the returned revision. Do not manipulate UI widgets
from a network task or mirror Preferences directly in JavaScript.

## Resource limits

- Web assets: 256 KiB gzip build budget; current build is reported by the local
  asset generator.
- Static requests: read from flash; browser performs gzip decompression.
- Live WebSocket: five clients, 2 Hz, 64-byte frames; it retains 60 frames
  (3.84 KiB) for initial replay. A sixth client receives a `limit:5` text
  notice and is closed. A queued send drops the next frame instead of building
  an unbounded backlog.
- Remote viewer: uses full-resolution BMP snapshots. A 320×480 capture uses
  about 450 KiB of temporary PSRAM and is rate-limited to four captures/second.
- History remains an asynchronous, bounded job in `history_query_service`;
  do not add a handler that scans files synchronously on the network/LVGL path.

## Next web milestones

1. Add explicit raw-file export UI.
2. Expand the Info page from additional existing service read models.
3. Add validated browser writes one domain at a time, with revision-aware
   refreshes.
4. Migrate port-80 OTA/static routes to ESP-IDF HTTPD when preserving the
   existing updater semantics has dedicated verification coverage.
