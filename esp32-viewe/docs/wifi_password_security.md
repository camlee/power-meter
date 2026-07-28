# Wi-Fi password handling security plan

## Purpose

Prevent a previously saved Wi-Fi password from crossing the Wi-Fi subsystem's
secret boundary. Saved station and access-point passwords must never be returned
by an HTTP API, populated into a browser or LVGL widget, displayed on screen, or
included in logs or error messages.

The firmware still needs internal access to saved credentials so it can reconnect
to a station network and restart a configured access point. This plan therefore
does not attempt to make credentials non-retrievable inside `network_manager`; it
removes general-purpose read access and exposes only narrow operations that use
the credentials internally.

## Scope

This work covers:

- removing the saved AP password from `GET /api/v1/wifi`;
- ensuring no HTTP response echoes a submitted or saved Wi-Fi password;
- removing public password-read APIs from `network_manager`;
- stopping the LVGL station password dialog from pre-filling a saved password;
- stopping the LVGL AP screen from putting a saved password in a text area;
- keeping browser password inputs blank until the user explicitly enters a new
  password;
- making saved-password state visibly non-editable until an explicit Replace
  action is selected;
- defining unambiguous keep, replace, and remove semantics for AP credentials;
- clearing temporary UI password state after submit, cancel, navigation, or
  refresh where practical;
- updating documentation and adding regression coverage for the new contract.

## Explicit non-goals

Do not include any of the following in this change:

- HTTP or WebSocket authentication or authorization;
- HTTPS, certificates, session management, CSRF, CORS, or network-origin checks;
- changes to the trusted-LAN deployment assumption;
- changes to initial provisioning or the default AP lifecycle;
- NVS encryption, flash encryption, Secure Boot, eFuses, JTAG, or physical-access
  protections;
- a new Wi-Fi provisioning protocol;
- changing WPA/WPA2/WPA3 support;
- changing the existing ESP32 password length rules, except where necessary to
  apply the current 8-to-63-byte validation consistently;
- unrelated Wi-Fi recovery, scan, connection, or UI redesign work.

## Current behavior to remove

### HTTP API

`webWifi()` loads the saved AP password and serializes it as
`ap.password` in `GET /api/v1/wifi`. The embedded browser polls this response
while the Wi-Fi page is visible, so the saved password is copied into the HTTP
response, browser network tools, JavaScript state, and an input value.

### LVGL station UI

`showPasswordPrompt()` calls `network_manager::getSavedPassword()` and places the
result in the masked password text area. Masking only changes rendering; the
saved plaintext is still present in the widget.

### LVGL AP UI

The AP screen calls `getSavedApSettings()` with a password output buffer and
places the saved password in `apPasswordInput`. That text area is not currently
configured for password rendering, so an active AP can display the saved
password in plaintext. The remote screenshot API can capture anything rendered
on this screen.

### Service boundary

`network_manager.h` publicly exposes `getSavedPassword()` and a
password-returning `getSavedApSettings()`. UI and API code can therefore retrieve
credentials even when they only need to know whether a credential exists.

## Target security contract

1. A read API may report that a password is configured, but never its value.
2. A write API may accept a new password, but no success or error response may
   echo it.
3. A saved password is represented in UI as status, not as an input value. Use
   text such as `Saved password (not shown)` rather than bullets in an input.
4. Do not use a variable number of bullets because that can disclose password
   length.
5. Password replacement always begins with a blank input.
6. Saved credentials are consumed only by narrow `network_manager` operations
   such as connecting a saved station or starting an AP while preserving its
   saved password.
7. Internal password-loading helpers remain private to
   `network_manager.cpp`. They must not be declared in `network_manager.h`.
8. Serial logs, ESP logs, diagnostics, screenshots, errors, and API responses
   must not contain saved or newly submitted passwords.
9. The embedded SPA and firmware API are released atomically, so this internal
   V1 response/request contract may change without retaining the insecure
   password-return behavior.

## Proposed API contract

### Read model

Remove `ap.password` from `GET /api/v1/wifi`. Return only metadata:

```json
{
  "ap": {
    "enabled": true,
    "ssid": "meter-example",
    "secure": true,
    "password_configured": true,
    "ip": "192.168.4.1",
    "client_count": 0,
    "clients": []
  }
}
```

`password_configured` is false for an open AP or when no usable saved AP
password exists. Do not return a masked password, empty `password`, or
`password: null`.

Station saved-network entries continue to contain SSIDs only. They must not gain
password, password length, hash, or masked-password fields.

### AP write model

Make credential intent explicit instead of treating an empty password as both
"keep" and "clear":

```json
{
  "enabled": true,
  "ssid": "meter-example",
  "secure": true,
  "password_action": "keep"
}
```

```json
{
  "enabled": true,
  "ssid": "meter-example",
  "secure": true,
  "password_action": "replace",
  "password": "new-password-value"
}
```

```json
{
  "enabled": true,
  "ssid": "meter-example",
  "secure": false,
  "password_action": "remove"
}
```

Rules:

- `keep` is valid only when `secure` is true and a valid saved AP password
  already exists.
- `replace` requires a password from 8 through 63 bytes when `secure` is true.
- `remove` is valid only when `secure` is false and removes the persisted AP
  password.
- Disabling the AP requires only `{"enabled":false}` and preserves the saved AP
  configuration, matching the current stop/start behavior.
- An open AP must never retain a password as active configuration.
- Unknown actions, contradictory fields, or a `keep` request without a saved
  password return a generic validation error that contains no credential value.
- Successful responses remain status-only, for example `{"ok":true}`.

If implementation constraints make this request shape unnecessarily invasive,
separate `startApKeepingPassword()` and `startApWithReplacementPassword()`
commands are acceptable internally. The HTTP request must still express whether
the caller intends to keep, replace, or remove the credential.

## UI behavior

### Browser station UI

- The station password input always starts blank.
- Selecting a scanned network clears the password input.
- Connecting a saved network continues to use `connect_saved`; it does not fetch
  the password.
- After a connect request succeeds or fails, clear `stationPassword`.
- Clear it when leaving the Wi-Fi route and when selecting another network.
- Do not derive browser draft cleanliness from any password returned by the
  device.
- Prefer `autocomplete="new-password"` for this write-only device credential
  field so a maintenance browser is not encouraged to inject a previously saved
  website password. Autocomplete is not a security boundary.

### Browser AP UI

- On load, show `Saved password (not shown)` when
  `password_configured` is true.
- The password control is disabled or absent by default and contains no value.
- An explicit `Replace password` action enables a blank password input and sets
  `password_action` to `replace`.
- Cancelling replacement clears and disables the input.
- Keeping other AP settings unchanged, stopping the AP, or re-enabling it uses
  `password_action: keep` when a saved secured credential exists.
- Switching from secured to open uses `password_action: remove`.
- Switching from open to secured requires replacement and a new valid password.
- After apply succeeds or fails, clear the password draft. A failed validation
  may leave the editor open, but it must not repopulate from device state.
- AP dirty-state comparison must use enabled/SSID/secure metadata and the local
  replacement state, not comparison with a saved password.

### LVGL station UI

- Remove the call that retrieves and pre-fills the saved station password.
- A secured scan result with a saved credential should use the existing
  saved-connect operation directly, or present a choice between `Use saved
  password` and `Replace password`; neither path may reveal the saved value.
- A replacement password prompt always starts blank and remains in LVGL password
  mode.
- Set the text area's maximum length to 63 and apply existing validation before
  attempting the connection.
- Clear the text area before deleting the overlay on connect or cancel.
- The Saved Networks overlay continues to connect by SSID through
  `connectSavedNetwork()`.

The simpler direct-connect behavior is preferred for this targeted change.
Users can Forget the credential and enter a replacement if authentication later
fails.

### LVGL AP UI

- Replace the saved password text area state with a non-secret status label.
- Show `Saved password (not shown)` when a secured credential exists.
- Provide an explicit Replace action that opens or enables a blank password text
  area.
- Put the replacement input in LVGL password mode and cap it at 63 bytes.
- While the AP is running, saved credential status is visibly read-only.
- Stopping and restarting an unchanged secured AP keeps the internal saved
  credential without loading it into the widget.
- Switching to open removes the stored AP password.
- Clear the replacement text area after apply or cancel.

## `network_manager` changes

Refactor the public interface so password reads cannot spread to other
subsystems:

- remove `getSavedPassword()` from `network_manager.h`;
- rename its implementation to a private helper such as
  `loadSavedStationPassword()` inside `network_manager.cpp`;
- use the private helper only for saved-station connection and automatic
  recovery;
- replace the password-returning `getSavedApSettings()` with a metadata query
  that reports SSID, enabled, secure, and `passwordConfigured`;
- keep the full AP credential loader private to `network_manager.cpp`;
- add an explicit AP password-action enum or equivalent narrow commands;
- validate action, SSID, secure mode, and password length in
  `network_manager`, not only in the web or LVGL callers;
- ensure `saveApSettings()` does not retain a password when the AP is open;
- preserve the existing behavior that a new station credential is saved only
  after a successful station connection;
- clear stack/temporary password buffers with a non-optimizable clearing
  function where the platform provides one. Do not add large architectural work
  solely to guarantee removal of every allocator copy.

The existing runtime copies needed for active connection and automatic recovery
remain in scope as internal secrets; this plan does not require redesigning the
Wi-Fi driver's credential lifetime.

## Expected files

The implementation will likely touch:

- `src/network/network_manager.h`
- `src/network/network_manager.cpp`
- `src/network/web_api.cpp`
- `src/ui/screens/settings/wifi_screen.cpp`
- `web/src/App.svelte`
- `web/src/lib/api.js` if request helpers or documentation need adjustment
- `docs/WEB_APP.md`
- `README.md` where current trusted-LAN/password behavior is described
- native or web test files added for password-action and draft-state behavior

Generated `src/network/web_assets.generated.*` files should continue to be
produced by the existing web build; do not edit them manually.

## Implementation sequence

1. Introduce the private credential loaders and metadata-only public queries in
   `network_manager`.
2. Add explicit AP keep/replace/remove behavior and central validation.
3. Change the HTTP Wi-Fi read/write models and confirm no response path contains
   a password.
4. Update the browser draft model and UI.
5. Update the LVGL station and AP flows.
6. Add regression tests and complete manual device checks.
7. Update documentation and rebuild/verify embedded web assets.

Keep intermediate commits buildable where practical. In particular, change the
firmware API and embedded SPA in the same release so the AP editor never
mistakes an omitted password for an empty replacement.

## Automated regression coverage

At minimum, add coverage for:

- AP password-action validation: valid and invalid keep/replace/remove
  combinations;
- replacement length boundaries of 7, 8, 63, and 64 bytes;
- keeping a secured AP credential without returning it to the caller;
- removing a password when switching an AP to open mode;
- rejecting `keep` when no saved credential exists;
- browser AP draft initialization with a blank password and
  `password_configured: true`;
- browser replacement/cancel/apply transitions clearing the local password;
- station selection and completed connect attempts clearing the browser
  password;
- parsed `GET /api/v1/wifi` output containing no `password` property and no known
  seeded test secret;
- successful and failed AP write responses containing no submitted test secret.

Prefer parsing JSON and checking exact property names. Do not use a blanket
substring rejection for `password`, because `password_configured` is an allowed
metadata property.

If the existing native test target cannot cheaply instantiate the full Arduino
HTTP layer, extract only the small AP credential-action validation policy into a
pure helper for native tests. Do not turn this targeted work into a web-server
rewrite.

## Manual acceptance checks

Use distinctive test-only station and AP passwords so leaks are easy to detect.
Do not use a real site password for testing.

1. Save station and AP credentials, reboot, and verify automatic station
   reconnect still succeeds.
2. Call `GET /api/v1/wifi`; confirm it reports only
   `password_configured` and does not contain either test password.
3. Inspect browser developer tools, JavaScript state, and password input values
   after page load and several poll cycles; confirm no saved password appears.
4. Open the LVGL station password prompt for a saved secured SSID; confirm the
   field is blank.
5. Open the LVGL AP screen and capture it through the remote screenshot API;
   confirm the password is represented only as non-secret saved status.
6. Stop and restart an unchanged secured AP without re-entering its password;
   confirm a client can reconnect with the existing password.
7. Replace the AP password; confirm the old password fails and the new password
   works.
8. Switch the AP to open and then back to secured; confirm open mode retained no
   reusable password and secured mode requires a new one.
9. Connect using a new station password; confirm the input is cleared after the
   request and the saved-network reconnect path still works after reboot.
10. Exercise validation failures, Wi-Fi reset, AP start failure, and station
    authentication failure; inspect all HTTP responses and serial logs for the
    test passwords.
11. Search the built web assets and captured API responses for the distinctive
    test secrets. The persisted firmware/NVS data used by the Wi-Fi subsystem is
    outside this particular check.

## Definition of done

This work is complete when:

- no saved password getter is exposed in `network_manager.h`;
- no HTTP GET response contains a saved Wi-Fi password;
- no HTTP write response or error echoes a submitted password;
- no browser or LVGL widget is populated with a previously saved password;
- existing-password state is shown only as fixed, read-only metadata;
- new password inputs begin blank and are cleared after their useful lifetime;
- saved station reconnect and secured AP stop/start continue to work;
- AP keep/replace/remove semantics are explicit and tested;
- remote screenshots and normal logs contain no Wi-Fi password;
- web, native, and firmware builds/tests relevant to the changed code pass; and
- documentation no longer states that the Wi-Fi read model includes the saved AP
  password.
