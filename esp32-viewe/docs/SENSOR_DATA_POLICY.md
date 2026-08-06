# Sensor data policy

The sensor layer separates what a source observed from what the meter is
allowed to calculate. A finite or plausible value is not proof that a sensor
is correctly installed or calibrated.

## Channels and states

The stable channels are `In`/Solar, `Out`/Load, and `Aux`/Battery. Presence is
explicit and independent for each channel. A missing channel is unavailable,
not zero; a floating ADC input is not attachment detection.

Each channel has one of these states:

| State | Meaning |
| --- | --- |
| `NotConfigured` | Unsupported or intentionally absent for the active source |
| `Waiting` | Configured, but no acceptable observation has arrived |
| `Valid` | Fresh finite voltage/current within the calculation limits |
| `OutOfRange` | Finite observation exists but violates a calculation limit |
| `Invalid` | Malformed, non-finite, or source-reported hardware failure |
| `Stale` | No acceptable observation arrived before the source timeout |

Source-specific presence rules remain in the source implementations. UART
presence comes from the frame mask; the ADC profiles currently expose their
configured physical channels. Consumers must use the state and configured
flag, not infer presence from a numeric value.

## Observations versus calculations

The inclusive calculation limits are defined in `src/sensors/sensor_limits.h`:

| Measurement | Minimum | Maximum |
| --- | ---: | ---: |
| Voltage | 0 V | 250 V |
| Current | -150 A | 150 A |
| Direct duty | 0 | 1 |

The Sensors UI and diagnostics may show finite out-of-range observations, with
their state clearly marked. Power, energy, and operational live values use only
eligible readings. Values are rejected rather than clamped; clamping would
fabricate power.

For physical ADC sources, high-rate samples are reduced into 500 ms readings.
A window is eligible only when at least 80% of its observations are valid;
rejected samples are excluded from the mean. The diagnostic path may retain a
finite out-of-range mean so installation problems remain visible.

Calibration is applied once per physical source/channel. Current direction is
applied after calibration, and logical remapping does not move or rewrite the
physical calibration profile. Demo and UART sources provide engineering units
and do not receive ADC calibration.

## Derived values

- Channel power is `voltage * current` and requires eligible voltage and current.
- User-facing system net power uses eligible Battery power first, with Solar
  minus Load as the fallback when Battery is unavailable.
- Battery charge/usage and duty-derived values require their own dependencies.
- A missing dependency yields unavailable output, never zero.

## Energy and history

Energy integrates only elapsed intervals bounded by eligible samples. It does
not bridge startup, stale timeouts, invalid/out-of-range intervals, or channel
absence. Negative power can produce negative energy.

History keeps energy and valid coverage separately for each channel and
derived component. Coverage distinguishes a configured channel that measured
zero from one that had no usable observation. A configured source may therefore
produce a minute with valid coverage for one channel and a gap for another;
an entirely unconfigured source produces no history rows.

The exact storage and query contract is in [HISTORY.md](HISTORY.md). The exact
source and mapping implementations are authoritative when this summary and
code ever disagree.
