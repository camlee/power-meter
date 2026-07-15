# Arduino Uno UART sensor bridge

## Purpose and status

The installed `arduino-lcd` meter can provide calibrated real-world readings to
the VIEWE ESP32-S3 while the final ESP32 ADC sensor hardware is unavailable.
The connection is one-way UART: the Uno remains responsible for its existing
sensor sampling, LCD, calibration, EEPROM, and energy display, while the ESP32
receives normalized readings and performs its own power and history processing.

This document defines the intended V1 hardware and wire contract. It is not yet
implemented or bench-verified. The Uno is not currently available, so changes
to that project must be conservative and testable without altering its core
meter behavior.

## Hardware connection

| Arduino Uno | VIEWE UEDX32480035E-WB-A J4 |
| --- | --- |
| TX / digital pin 1 | Pin 3, `UARTRX` / GPIO44, through the divider below |
| GND | Pin 4, GND |

Leave J4 pin 1 (`USB-5V`) and pin 2 (`UARTTX` / GPIO43) disconnected. The
boards keep their existing power arrangements and share only signal ground.

### Required 5 V to 3.3 V divider

The Uno TX output is 5 V logic and the ESP32-S3 input is not 5 V tolerant.

```text
Uno TX ---- R1 ----+---- VIEWE J4 pin 3 / GPIO44 RX
                   |
                   R2
                   |
                  GND
```

Use approximately `R1:R2 = 1:2`, for example 1 kOhm and 2 kOhm. Two identical
resistors in series for R2 and one of the same value for R1 also produce the
required ratio. Verify the assembled divider and common ground before attaching
it to the ESP32.

Uno pins 0/1 are shared with its USB programming interface. The receive-only
divider is expected to be a light load, but it should be disconnected during
flashing if upload or serial behavior is unreliable.

## UART settings and ownership

- 9600 baud, 8 data bits, no parity, 1 stop bit (`9600 8N1`).
- Uno transmits; ESP32 does not send commands or acknowledgements.
- One record every 500 ms (2 Hz).
- Uno sensor acquisition remains faster and independent from transmission.
- A record ends with LF (`\n`). A receiver may discard one preceding CR.
- Maximum record length, including checksum and LF: 160 bytes.

J4 GPIO44 is UART0 RX. With `ARDUINO_USB_CDC_ON_BOOT=0`, the Arduino `Serial`
object owns GPIO43/44, so the receiver uses that hardware path rather than
inventing a different pin mapping. UART RX must be consumed incrementally and
without `String`, `readStringUntil()`, or another timeout-based blocking call.

UART0 is also the current Arduino `Serial` diagnostic output. Its TX pin is not
connected in this bridge, while the board's USB-JTAG console remains available
for ESP-IDF logging. The implementation must audit important `Serial`-only
diagnostics and preserve a usable USB-JTAG debug/recovery path.

## V1 record

The V1 format is bounded ASCII so it is easy to capture from the Uno USB serial
interface, inspect by eye, and parse without dynamic allocation.

```text
PM1,<sequence>,<uno_ms>,<mask>,<in_v>,<in_a>,<in_duty>,<out_v>,<out_a>,<out_duty>,<aux_v>,<aux_a>,<aux_duty>*<crc16>\n
```

Example for the expected Uno configuration, with `In` and `Out` present and
`Aux` absent:

```text
PM1,4182,2759012,03,18.24,2.13,0.742,13.17,1.06,,,,*7200
```

The example checksum is an authoritative V1 test vector for the algorithm below.

### Fields

| Field | Contract |
| --- | --- |
| `PM1` | Protocol magic and version. |
| `sequence` | Unsigned 32-bit record counter, incremented once per transmitted record; wrap is allowed. |
| `uno_ms` | Unsigned 32-bit Uno `millis()` at snapshot time; wrap is allowed. |
| `mask` | Two uppercase hexadecimal digits. Bit 0 = `In`, bit 1 = `Out`, bit 2 = `Aux`; other bits must be zero. Expected Uno mask is `03`. |
| `*_v` | Calibrated voltage in volts. |
| `*_a` | Calibrated current in amps. Signed values are allowed by the protocol. |
| `*_duty` | Optional direct duty in the inclusive range 0–1. Empty asks the ESP32 to derive it when meaningful. |
| `crc16` | Four uppercase hexadecimal digits containing CRC-16/CCITT-FALSE. |

Voltage and current are required decimal finite numbers for a present channel;
its duty may be empty. All three fields for an absent channel are empty. `nan`,
`inf`, scientific text that overflows the receiver, partial voltage/current
data, extra fields, and trailing content are invalid.

The expected Uno sends direct duty for `In`, where its faster samples can
observe charger behavior more meaningfully than the ESP32's 2 Hz receiver.
Direct duty is optional for the other channels. Power and energy are deliberately
not transmitted: the ESP32 calculates `voltage * current` and integrates its
own history.

### Checksum

Use CRC-16/CCITT-FALSE:

- polynomial `0x1021`;
- initial value `0xFFFF`;
- no reflection;
- no final XOR;
- input bytes begin with `P` in `PM1` and end with the last byte before `*`.

The checksum protects framing and corrupted ASCII. It is not authentication.

## Transmitter behavior

The Uno implementation must:

- keep the current fast acquisition and moving-average behavior;
- take one coherent engineering-unit snapshot for each 500 ms record;
- calculate and emit `In` duty using the existing Uno algorithm/window;
- emit calibrated `In`/Panel and `Out`/Load values;
- mark `Aux` absent instead of sending zeroes;
- use a fixed-size stack buffer or direct bounded printing suitable for AVR RAM;
- never let serial backpressure change sensor integration timing;
- preserve LCD, buttons, calibration, EEPROM addresses, and existing local
  energy calculations.

The current `LOG_READINGS` implementation is not the V1 protocol. It transmits
too frequently, has no schema or presence metadata, and advances its ring index
before printing. It should be replaced by a separately scheduled telemetry
snapshot, not patched by depending on the current CSV field order.

## Receiver behavior

The ESP32 implementation must:

1. accumulate bytes into a fixed 160-byte buffer;
2. discard and count an overlong line until the next LF;
3. reject frames with invalid magic/version, field count, mask, checksum,
   numeric syntax, non-finite values, or incomplete voltage/current pairs;
4. apply the shared 0–120 V, -50–50 A, and 0–1 duty calculation policy without
   applying ESP32 ADC calibration; a finite out-of-range channel remains
   observable but ineligible for power/history;
5. publish all channels from one accepted record as one coherent snapshot;
6. use ESP32 receive time for live/history timing and Uno time/sequence only for
   source diagnostics;
7. detect duplicates, sequence gaps, Uno resets, and timeout/recovery;
8. expose valid/invalid counts, last error, last-valid age, mask, sequence, and
   Uno uptime through diagnostics;
9. stop publishing valid samples when input is stale so history records a gap.

The source begins in `Waiting`, becomes `Valid` after its first accepted record,
and becomes `Stale` when no valid record arrives for two seconds. A malformed
record increments diagnostics but does not immediately erase a still-fresh last
sample. Recovery occurs on the next valid record.

The transport carries observations; calculation eligibility is defined in
[SENSOR_DATA_POLICY.md](SENSOR_DATA_POLICY.md). A supplied out-of-range duty
invalidates duty-dependent metrics without discarding otherwise valid voltage,
current, and power.

## Verification without the installed Uno

- Build `arduino-lcd` for an Uno with PlatformIO and retain a Makefile wrapper.
- Unit-test CRC and parser behavior with shared valid/invalid text fixtures.
- Compile the transmitter's formatting logic for AVR and verify its maximum
  line length and memory use.
- Feed captured/generated records into the ESP32 parser, including partial
  chunks, CRLF, overflow, bad CRC, missing fields, non-finite numbers, sequence
  wrap/gaps, Uno reset, silence, and recovery.
- Review the Uno diff specifically for LCD/button/calibration/EEPROM/energy
  behavior before preparing a flash image.

Final acceptance requires the physical Uno: capture its output over USB, compare
UART values and duty to its LCD, verify divider voltage, exercise disconnect and
reconnect, and observe ESP32 history over a representative installed interval.
