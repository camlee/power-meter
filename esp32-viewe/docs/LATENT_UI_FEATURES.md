# Latent UI features

These features remain implemented but are intentionally absent from one or
more current MPPT-oriented interfaces. Do not remove their services, stored
fields, or API contracts when simplifying the UI.

## Stable channel identifiers

The internal, UART, history, REST, and WebSocket identifiers remain `in`,
`out`, and `aux` for compatibility. Their user-facing roles are:

| Stable ID | Display name | Power convention |
| --- | --- | --- |
| `in` | Solar | Positive production |
| `out` | Load | Positive consumption |
| `aux` | Battery | Positive charge; negative discharge |

User-facing system net power is positive while charging. It uses
`Battery` when Battery is eligible and falls back to `Solar - Load`.
The legacy `net_battery_power` API field retains its `in - out` definition.

## Cycle

- The LVGL Cycle page is not registered in top-level navigation.
- Its screen, energy model, history query, and `/api/v1/cycles` endpoint remain.
- The browser Cycle page remains visible and supported.
- Restore the LVGL page by registering `cycle_screen::create` in
  `src/ui/navigation/app_navigation.cpp`.

## PWM duty and surplus

`POWER_METER_CONTROLLER_MODE_PWM` selects the controller presentation:

- `0` (default): MPPT UI. Duty, PWM percentages, and calculated Surplus are
  hidden from LVGL and the browser.
- `1`: PWM UI. Duty diagnostics and the legacy surplus-oriented Usage
  presentation are visible.

Both current PlatformIO targets explicitly default to MPPT. The status API
publishes `capabilities.controller_mode` and `capabilities.pwm_ui` so the one
embedded browser build adapts at runtime.

The following remain active in MPPT mode:

- optional direct duty ingestion;
- derived duty and available-power functions;
- UART/WebSocket/REST duty fields;
- ADC raw-capture duty values;
- historical component storage, including `PANEL_SURPLUS`;
- all legacy history response fields.

Only their UI presentation is disabled.

## Web remote display

Remote display/control remains a sixth browser Settings page after Debug. It
is capability-gated by `touch_display` and has no LVGL equivalent.
