# Embedded web application

The Svelte application is embedded in program flash and is delivered in the
same signed firmware image as the API it uses. It is a second consumer of the
same runtime services as LVGL, not an independent calculation or settings
model.

## Build and delivery

```sh
cd esp32-viewe/web
npm ci
npm run check
```

PlatformIO runs the web build automatically before a firmware build. The
generated C++ asset table is ignored source output and becomes part of
`firmware.bin`; LittleFS is reserved for history. The gzip payload budget is
256 KiB, and each target must still fit its OTA application slot.

Static files are served from flash:

- the HTML shell is revalidated (`no-cache`, ETag);
- hashed JavaScript/CSS assets are immutable and long-lived;
- API, status, writes, and display captures are `no-store`.

There is no service worker. Revalidating the shell after OTA points the browser
at the new hashed assets while compatible cached assets remain reusable.

## Finding and trusting a device

The firmware prints its current station/AP URL and hostname when networking
changes. mDNS is advertised through HTTP and OTA services. Use:

```sh
python3 tools/discover_device.py
python3 tools/discover_device.py --host 192.168.1.217
```

The numeric IP is the reliable fallback on multicast-isolated networks and
direct AP connections.

The browser-facing service is unauthenticated and intended for a trusted LAN.
Wi-Fi passwords are write-only: read models report only whether an AP password
is configured, and successful or failed writes never echo credentials. Remote
display/control has the same trusted-LAN boundary. OTA authenticity comes from
the embedded signing key, not browser authentication. Do not expose these
endpoints to an untrusted network without adding authentication and transport
protection.

## Service boundaries and endpoints

Port 80 uses the Arduino HTTP server for the SPA, JSON APIs, history jobs, and
signed OTA routes. Port 81 uses the native HTTP server for the server-push
WebSocket at `/api/v1/live`. The WebSocket is capped at five clients and sends
at 2 Hz; a new client receives up to 60 replay frames first.

The main public surfaces are:

| Surface | Purpose |
| --- | --- |
| `/api/v1/web/status`, `/api/v1/info`, `/api/v1/debug` | Capabilities, identity, health, and diagnostics |
| `/api/v1/sensors`, `/api/v1/sensors/mapping`, `/api/v1/sensors/calibration` | Sensor state, mapping, calibration, and raw diagnostics |
| `/api/v1/setup`, `/api/v1/wifi`, `/api/v1/time/anchor` | Shared setup, Wi-Fi commands, and browser time contribution |
| `/api/v1/history/query`, `/api/v1/history/files` | Asynchronous history query and dataset catalog |
| `/api/v1/cycles` | Asynchronous daily energy-cycle summaries and settings |
| `/api/v1/display/...` | Capability-gated screenshot and pointer control |
| `/api/v1/updates...` | Signed Internet update status and commands |

The exact route validation and JSON fields live in `src/network/web_api.cpp`.

## Versioned binary protocols

The browser parser in `web/src/lib/api.js` is the normative consumer for binary
responses; the firmware encoders contain matching static size/layout checks.
Change both sides together.

- Live replay frames are V4, 84 bytes, logical channels only.
- Current live frames are V5, 128 bytes, with physical Sensor 1/2/3
  diagnostics appended after the V4 fields.
- History responses are VPH3 with an 80-byte bucket and explicit timeline,
  quality, configured-mask, and per-channel/component coverage fields.
- History and cycle queries are asynchronous jobs; clients start with a GET,
  receive a job ID, and poll until the result is ready.

Unavailable readings are represented as state/null/NaN according to the
surface, never as fabricated zeroes. The browser must preserve the distinction
between configured, observed, calculation-eligible, and stale/out-of-range
data.

## Shared state and timing

LVGL and Web use the same persistence and service methods. A successful
mutation calls the owning service and increments `device_state` where relevant;
network callbacks do not touch LVGL objects. The browser refetches the small
status model when the live state revision changes.

The browser submits its local time with `POST /api/v1/time/anchor`:

```json
{"unix_ms":1783890123456,"utc_offset_minutes":-360}
```

The firmware owns anchoring, gap handling, calendar boundaries, and history
aggregation. The frontend renders the returned timeline and coverage flags.

## Remote display

Touch-enabled builds expose the LCD framebuffer and pointer input through the
browser remote page and `/api/v1/display/...`. A full-resolution 320×480
capture temporarily uses roughly 450 KiB of PSRAM and the service is
rate-limited. The feature is capability-gated and absent on WROOM.
