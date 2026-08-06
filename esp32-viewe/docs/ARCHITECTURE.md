# Architecture

`esp32-viewe` is an offline-first power meter. Measurement, local UI, and
history must continue to work without Wi-Fi, NTP, or a peer. One application
runtime supports two hardware profiles; compile-time capabilities let the
shared web application adapt to each target.

## Runtime shape

```text
Demo / ESP32 ADC / ADS1115 / UART sources
                    |
                    v
physical channels -> calibration -> direction -> logical mapping
                    |
                    v
             sensor states and derived power
                 /       |        \
                v        v         v
             local UI  history   Web/API/diagnostics
                         |
                         v
               monotonic records + time anchors

Wi-Fi, NTP, browser time, OTA, and remote display are supporting services.
They must not be prerequisites for the measurement path.
```

## Service ownership

| Area | Owns | Must not do |
| --- | --- | --- |
| `src/sensors/` | Source acquisition, calibration, current direction, physical-to-logical mapping, state, short histories, and derived readings | Read files or manipulate UI objects |
| `application_runtime` | Startup order, periodic service updates, and conversion of eligible readings into history frames | Put hardware-specific policy in UI code |
| `src/data/` | Energy accumulation, durable minute records, dataset routing, catalog, retention, and queries | Invent values for missing coverage |
| `src/time/` | Boot sessions, wall-clock anchors, fixed-offset calendar mapping, and uncertainty | Make sensing depend on wall time |
| `network_manager` | Station/AP credentials, Wi-Fi state, reconnect behavior, and NTP reachability | Expose saved credential values |
| `src/network/` | HTTP/WebSocket read models, commands, OTA, and remote display | Touch LVGL directly from network callbacks |
| `src/ui/` and `web/` | Present state and issue service commands | Maintain a second settings or calculation model |

The local touchscreen and browser are two consumers of the same services.
Changes made by either surface persist through the owning service and notify
the shared `device_state` revision where appropriate.

## Measurement invariants

The stable channel identifiers are `In`, `Out`, and `Aux`, presented as Solar,
Load, and Battery. Solar and Load are positive in their respective directions;
Battery is positive while charging and negative while discharging. A physical
sensor is calibrated before current direction is applied, so remapping or
reversing a channel does not alter stored calibration values or raw readings.

Presence/configuration is separate from value validity. A floating ADC input is
not evidence that a sensor is attached. The sensor layer publishes explicit
states and consumers decide whether a value is calculation-eligible; see
[SENSOR_DATA_POLICY.md](SENSOR_DATA_POLICY.md).

Power and energy are calculated only from eligible readings. Missing, stale,
invalid, or out-of-range data produces unavailable values and history gaps,
not zeroes or carried-forward samples.

## Time and history invariants

Every boot has a persistent nonzero monotonic session ID. Measurement order is
therefore available before wall time exists. NTP and browser time add anchors
separately; they do not rewrite measurement rows or make the current session
depend on the network. Calendar views use the persisted fixed UTC offset.

Real and Demo history are isolated datasets using one storage/query engine.
The active source determines where new records go. The Settings Data view may
inspect either catalog, but it never changes recording destination. Details of
the format, recovery, fixtures, retention, and query-time inference are in
[HISTORY.md](HISTORY.md).

## Concurrency and reliability

- Sampling and reducer work run independently of LVGL and network callbacks.
- Background tasks do not manipulate LVGL objects directly; UI work follows
  the LVGL port locking conventions.
- Recurring UI work is visibility-aware and must remain bounded and
  non-blocking.
- History writes are buffered and scheduled so flash latency cannot starve
  acquisition.
- A sensor, clock, Wi-Fi, or peer failure degrades the affected feature but
  does not stop local measurement.

## Hardware profiles

- **VIEWE:** ESP32-S3 touch display, LVGL, PSRAM, and built-in ADC. The current
  physical defaults are Sensor 1 voltage/current GPIO 6/5, Sensor 2 GPIO 10/9,
  and Sensor 3 GPIO 8/7. Runtime mapping assigns these physical channels to
  logical roles.
- **WROOM:** ESP32-WROOM, web-first, no LVGL, SSD1306 at `0x3c` and ADS1115 at
  `0x48` on SDA GPIO 5 / SCL GPIO 4. The display and ADC share one serialized
  I2C service.

The build configuration is authoritative for capabilities and defaults. Do not
copy hardware definitions into UI or network code.

## Deliberate boundaries

Peer/mesh aggregation, automatic physical-sensor detection, full IANA
timezone/DST history, and internet-facing authentication are not current
product dependencies. The local web and remote-display endpoints are intended
for a trusted LAN; OTA authenticity comes from signed images, not browser
authentication. See [WEB_APP.md](WEB_APP.md) and [OTA.md](OTA.md).
