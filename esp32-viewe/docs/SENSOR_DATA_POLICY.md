# Sensor data and calculation policy

## Purpose

Sensor acquisition, display, calculation, and history have different trust
requirements. This policy lets developers see what the hardware actually
reported without allowing an impossible, stale, or missing observation to
fabricate power or energy.

The policy applies equally to realtime Demo, ESP32 ADC, and UART
sources. Passing plausibility checks means only that a value is eligible for
calculation; it does not prove that a sensor is correctly installed or
calibrated.

## Channel configuration and state

`In`, `Out`, and `Aux` have independent presence/state. The completed runtime
contract accepts the channels a source supports or currently advertises. The
remaining configuration follow-up will make effective configuration the
intersection of two masks:

- the channels a source supports or currently advertises;
- the channels locally enabled for that source in device Setup.

A channel must not ultimately be configured merely because an ADC pin produced
a value; floating pins are not attachment detection. Persisted per-source local
enable masks are not implemented yet. Until they are, the runtime behavior is:

| Source | Current effective presence |
| --- | --- |
| Realtime Demo | `In`, `Out`, and `Aux` are configured. |
| UART | Each valid frame's advertised mask is authoritative. |
| ESP32 ADC | All three provisional channels are configured pending installer enable-mask support. |

The follow-up Setup control will persist masks independently so switching
sources does not overwrite another source's choices. It may disable a
UART-advertised, ADC, or Demo channel, but may not enable a channel the source
does not advertise. UART disappearance from an otherwise valid frame is already
a presence/configuration diagnostic and is never treated as a zero observation.

Each configured channel has one of these runtime states:

| State | Meaning |
| --- | --- |
| `Waiting` | The source has not supplied its first syntactically valid observation. |
| `Valid` | Voltage/current are finite, fresh, and eligible for calculations. |
| `OutOfRange` | Finite observations exist but violate calculation limits. |
| `Invalid` | The source supplied malformed or non-finite observations, or reported a hardware error. |
| `Stale` | No acceptable observation arrived within the source's timeout. |
| `NotConfigured` | The channel is unsupported or intentionally disabled for this source. |

Source-level diagnostics additionally retain the available last-valid age,
invalid/rejected counts, and source-specific errors. A malformed UART
frame is a source transport error; an individually implausible parsed value is
a channel data-quality error.

## Observation and display policy

The Sensors screen is diagnostic and shows the latest finite engineering-unit
observation even when it is outside calculation limits. It must mark the value
and channel state clearly rather than silently clipping it. Calibration preview
also retains raw ADC input and the unbounded converted observation.

- A finite negative voltage or 150 V observation may be shown on Sensors as
  observed, with `Out of range` state.
- Non-finite values are shown as `Invalid`, not formatted as a number or added
  to charts.
- `NotConfigured`, `Waiting`, and `Stale` show `--` for a current calculated
  value; an optional last-valid value must be explicitly labeled as stale.
- Charts protect their numeric ranges from overflow but do not replace an
  out-of-range observation with a boundary value.

This raw visibility is important during calibration and on-site diagnosis.

## Calculation eligibility

The initial inclusive calculation limits are:

| Measurement | Minimum | Maximum |
| --- | ---: | ---: |
| Voltage | 0 V | 120 V |
| Current | -50 A | 50 A |
| Direct duty | 0 | 1 |

Values inside these bounds include expected 12–14 V and 20–40 V systems, zero
current, approximately -0.5 A solar back-leakage, and the rare case of a sensor
installed backward. Voltage/current limits are centrally defined so the final
hardware milestone can refine them from electrical ratings without scattering
constants through sources or UI code.

Power is calculated only when voltage and current are both eligible:

```text
power_W = voltage_V * current_A
```

The resulting initial power range is therefore -6,000 W to 6,000 W. Values are
never clamped to those boundaries: clamping would fabricate power and energy.
An out-of-range input makes calculated power unavailable until a valid sample
arrives.

Direct duty is independently optional. Missing or invalid duty does not discard
otherwise valid voltage/current/power; duty-dependent available-power metrics
are derived from eligible history where meaningful or shown unavailable.

## Derived metrics

- Channel power requires that channel's valid voltage and current.
- Net battery power requires valid `In` and `Out`; `Aux` is excluded.
- Battery charging/usage components require the same valid inputs used by their
  formulas.
- Panel duty/available power requires valid `In` power plus valid direct or
  defensibly derived duty.
- A missing dependency produces unavailable output, never zero.

The Power screen and public live API use only eligible calculations. They may
link back to the Sensors screen/source diagnostics when a value is unavailable.

## Energy and history

Energy integration uses only elapsed intervals bounded by eligible samples. It
does not bridge source startup, stale timeouts, invalid/out-of-range intervals,
or channel absence.

When no channel is effectively configured, history does not create empty rows
or files. Once any channel is configured, minute rows may retain zero coverage
for a configured-but-failed channel alongside valid coverage for other channels.

History records, per channel and derived component:

- accumulated energy for the valid portion of each minute;
- valid coverage milliseconds for that minute;
- which channels were configured;
- minute quality flags indicating rejected or stale intervals.

No cumulative energy cap is applied. Negative power may produce negative
channel energy. Coverage, rather than a zero value, distinguishes genuine zero
power from missing data. Queries propagate per-channel/component coverage so a
partially configured device can retain useful history without claiming that an
absent channel measured zero.

## Calibration implications

Plausibility filtering is not calibration validation. A miscalibrated reading
inside the broad limits remains eligible and may be historically wrong. The
future on-site milestone must establish calibration defaults, reference
equipment, and acceptance tolerances.

Until then:

- ADC observations expose raw millivolts and calibrated engineering values;
- UART observations are treated as already calibrated by the source producer;
- source provenance and calibration state remain visible in diagnostics;
- development history can be wiped without migration before candidate V1 is
  treated as production data.
