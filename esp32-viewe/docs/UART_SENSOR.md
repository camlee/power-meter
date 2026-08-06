# UART sensor interface

The UART source accepts calibrated engineering-unit readings from an external
producer. The ESP32 owns the normalized power calculation, validation, live
state, and history integration; the producer owns its own sampling and
calibration. The connection is one-way.

## Physical connection

| Arduino Uno | VIEWE UEDX32480035E-WB-A J4 |
| --- | --- |
| TX / digital pin 1 | Pin 3, `UARTRX` / GPIO44, through a divider |
| GND | Pin 4, GND |

Leave J4 pin 1 (`USB-5V`) and pin 2 (`UARTTX` / GPIO43) disconnected. The Uno
TX signal is 5 V and the ESP32-S3 input is not 5 V tolerant:

```text
Uno TX ---- R1 ----+---- GPIO44 RX
                   |
                   R2
                   |
                  GND
```

Use approximately `R1:R2 = 1:2`, such as 1 kΩ and 2 kΩ. Verify the divider and
common ground before connecting the boards. Disconnect the bridge during Uno
flashing if it interferes with the USB serial path.

## Transport

- `9600 8N1`
- Uno transmits; the ESP32 sends no commands or acknowledgements
- one record every 500 ms
- each record ends in LF; CR immediately before LF may be accepted
- maximum record length, including checksum and LF, is 160 bytes
- UART RX is consumed incrementally with a fixed buffer; it must not block on
  a timeout or allocate per line

The Uno uses UART0 TX/RX pins for this bridge. Its TX is not connected back to
the Uno, while the VIEWE USB-JTAG console remains the recovery/debug path.

## V1 record

```text
PM1,<sequence>,<source_ms>,<mask>,<in_v>,<in_a>,<in_duty>,<out_v>,<out_a>,<out_duty>,<aux_v>,<aux_a>,<aux_duty>*<crc16>\n
```

Example with In and Out present and Aux absent:

```text
PM1,4182,2759012,03,18.24,2.13,0.742,13.17,1.06,,,,*7200
```

The checksum is CRC-16/CCITT-FALSE over the bytes beginning with `P` and
ending immediately before `*`: polynomial `0x1021`, initial `0xFFFF`, no
reflection, and no final XOR. The example is a test vector.

| Field | Meaning |
| --- | --- |
| `sequence` | Unsigned 32-bit producer record counter; wrap is allowed |
| `source_ms` | Unsigned 32-bit producer uptime at the snapshot; wrap is allowed |
| `mask` | Two uppercase hex digits: bit 0 In, bit 1 Out, bit 2 Aux; other bits zero |
| `*_v`, `*_a` | Finite calibrated voltage/current for each present channel |
| `*_duty` | Optional direct duty in `0..1`; empty means derive when meaningful |
| `crc16` | Four uppercase hexadecimal checksum digits |

All three fields for an absent channel are empty. Partial voltage/current
pairs, non-finite values, overflowing/scientific numeric forms, extra fields,
and trailing content are invalid. Power and energy are not transmitted.

## Receiver behavior

The receiver rejects bad framing, field count, mask, checksum, numeric syntax,
and channel completeness without blocking the acquisition path. A valid frame
publishes all channels as one coherent snapshot. ESP32 receive time drives live
and history timing; producer sequence/uptime are diagnostic metadata.

The source transitions from `Waiting` to valid operation after its first
accepted frame and becomes `Stale` after two seconds without one. A malformed
frame increments diagnostics without immediately discarding a still-fresh
sample. The next valid frame recovers the source. Sequence gaps, producer
resets, parser errors, last-valid age, the advertised mask, and bounded counters
are exposed through `/api/v1/sensors`.

Calculation eligibility follows
[SENSOR_DATA_POLICY.md](SENSOR_DATA_POLICY.md). An out-of-range duty invalidates
duty-dependent metrics but does not discard otherwise valid voltage/current/
power.

The implemented parser and its tests are the authoritative wire contract:
`src/sensors/pm1_uart_protocol.*`, the Uno producer, and
`test/test_pm1_uart_parser/` must be changed together.
