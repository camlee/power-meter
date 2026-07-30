<script>
  import { createEventDispatcher, onMount } from 'svelte';
  import { getSensorMapping, saveSensorMapping } from './api.js';
  import {
    cloneSensorMapping,
    mappingBalance,
    mappingIsValid,
    mappingsEqual,
    previewSensorMapping,
  } from './sensorMapping.js';

  export let sourceLabel = 'Sensor source';
  export let livePhysicalSensors = null;

  const dispatch = createEventDispatcher();
  const roles = [
    ['solar', 'Solar'],
    ['load', 'Load'],
    ['battery', 'Battery'],
    ['unmapped', 'None'],
  ];

  let saved = null;
  let draft = null;
  let loading = true;
  let busy = false;
  let error = '';

  $: preview = previewSensorMapping(saved, draft, livePhysicalSensors);
  $: balance = mappingBalance(draft, preview);
  $: valid = mappingIsValid(draft);
  $: dirty = !mappingsEqual(saved, draft);
  $: availablePowers = preview
    .filter((sensor) => sensor.available)
    .map((sensor) => Math.abs(sensor.power));
  $: powerDigits = availablePowers.some((power) => power >= 1000) ||
    (availablePowers.length === 3 && availablePowers.every((power) => power >= 10))
    ? 0 : 1;

  function describeError(err, action) {
    if (err?.userMessage) return err.userMessage;
    if (err?.status) return `Unable to ${action}: meter error ${err.status}.`;
    return `Unable to ${action}: ${err?.message || 'unknown error'}.`;
  }

  async function load() {
    loading = true;
    error = '';
    try {
      saved = await getSensorMapping();
      draft = cloneSensorMapping(saved);
    } catch (err) {
      error = describeError(err, 'load sensor mapping');
    } finally {
      loading = false;
    }
  }

  function replaceSensor(index, changes) {
    draft = {
      ...draft,
      physical_sensors: draft.physical_sensors.map((sensor, sensorIndex) =>
        sensorIndex === index ? { ...sensor, ...changes } : sensor),
    };
    error = '';
  }

  function changeRole(index, event) {
    replaceSensor(index, { role: event.currentTarget.value });
  }

  function changeDirection(index, current_direction) {
    replaceSensor(index, { current_direction });
  }

  function cancel() {
    if (busy) return;
    dispatch('cancel');
  }

  function calibrate(sensor, measurement) {
    if (busy || dirty || sensor.role === 'unmapped' ||
        sensor.calibration?.editable !== true) return;
    dispatch('calibrate', {
      role: sensor.role,
      measurement,
    });
  }

  async function save() {
    if (!valid || !dirty || busy) return;
    busy = true;
    error = '';
    try {
      await saveSensorMapping(draft);
      dispatch('restarting');
    } catch (err) {
      error = describeError(err, 'save sensor mapping');
      busy = false;
    }
  }

  function statusSymbol(sensor) {
    if (sensor.state === 'valid') return '✓';
    if (sensor.state === 'out_of_range' || sensor.state === 'invalid' ||
        sensor.state === 'stale') return '!';
    return '○';
  }

  function statusLabel(sensor) {
    const labels = {
      not_configured: 'Not configured',
      waiting: 'Waiting for reading',
      valid: 'Valid reading',
      out_of_range: 'Reading outside expected range',
      invalid: 'Invalid reading',
      stale: 'Stale reading',
    };
    return labels[sensor.state] || 'Waiting for reading';
  }

  function statusClass(sensor) {
    if (sensor.state === 'valid') return 'good';
    if (['out_of_range', 'invalid', 'stale'].includes(sensor.state)) return 'warning';
    return 'muted';
  }

  function measurement(value, unit, digits = 1, signed = false) {
    if (!Number.isFinite(value)) return `— ${unit}`;
    const sign = signed && value > 0 ? '+' : '';
    return `${sign}${value.toFixed(digits)} ${unit}`;
  }

  onMount(load);
</script>

<article class="mapping-editor" aria-labelledby="mapping-title">
  <header>
    <div>
      <h2 id="mapping-title">Sensor mapping</h2>
      <p>{saved?.source ? sourceLabel : 'Loading active source…'}</p>
    </div>
    <div class="header-actions">
      <span class="live-indicator" class:active={livePhysicalSensors?.length === 3}>
        {livePhysicalSensors?.length === 3 ? 'Live' : 'Waiting'}
      </span>
      <button type="button" class="close-button" disabled={busy}
        aria-label="Close sensor mapping" title="Close"
        on:click={cancel}>×</button>
    </div>
  </header>

  {#if error}<p class="mapping-error" role="alert">{error}</p>{/if}

  {#if loading}
    <p class="empty">Loading sensor mapping…</p>
  {:else if draft}
    <div class="sensor-list">
      <div class="sensor-column-headings" aria-hidden="true">
        <span>Sensor</span>
        <span>Role</span>
        <span>Current direction</span>
      </div>
      {#each preview as sensor, index (sensor.id)}
        <section class="sensor-card">
          <div class="sensor-card-heading">
            <h3>{sensor.label || `Sensor ${index + 1}`}</h3>
          </div>

          <div class="sensor-controls">
            <label>
              <span class="visually-hidden">Role for {sensor.label}</span>
              <select value={sensor.role} disabled={busy}
                on:change={(event) => changeRole(index, event)}>
                {#each roles as role}
                  <option value={role[0]}>{role[1]}</option>
                {/each}
              </select>
            </label>

            <fieldset disabled={busy}>
              <legend class="visually-hidden">
                Current direction for {sensor.label}
              </legend>
              <div class="direction-segments">
                <button type="button"
                  class:active={sensor.current_direction === 'normal'}
                  aria-pressed={sensor.current_direction === 'normal'}
                  aria-label="Use current as read"
                  on:click={() => changeDirection(index, 'normal')}>+</button>
                <button type="button"
                  class:active={sensor.current_direction === 'reversed'}
                  aria-pressed={sensor.current_direction === 'reversed'}
                  aria-label="Reverse current direction"
                  on:click={() => changeDirection(index, 'reversed')}>−</button>
              </div>
            </fieldset>
          </div>

          <div class="sensor-readings" aria-label={`${sensor.label} live readings`}>
            <span class="sensor-state {statusClass(sensor)}"
              title={statusLabel(sensor)} aria-label={statusLabel(sensor)}>
              {statusSymbol(sensor)}
            </span>
            <span class="editable-reading">
              {measurement(sensor.voltage, 'V', 0)}
              <button type="button" class="edit-reading"
                disabled={busy || dirty || sensor.role === 'unmapped' ||
                  sensor.calibration?.editable !== true}
                aria-label={`Calibrate ${sensor.label} voltage`}
                title="Calibrate voltage"
                on:click={() => calibrate(sensor, 'voltage')}>✎</button>
            </span>
            <span class="editable-reading">
              {measurement(sensor.current, 'A', 0, true)}
              <button type="button" class="edit-reading"
                disabled={busy || dirty || sensor.role === 'unmapped' ||
                  sensor.calibration?.editable !== true}
                aria-label={`Calibrate ${sensor.label} current`}
                title="Calibrate current"
                on:click={() => calibrate(sensor, 'current')}>✎</button>
            </span>
            <span>{measurement(sensor.power, 'W', powerDigits, true)}</span>
          </div>
          <p class:warning={sensor.interpretation.warning}
            class="interpretation">{sensor.interpretation.text}</p>
        </section>
      {/each}
    </div>

    {#if !valid}
      <p class="mapping-validation" role="alert">
        Assign Solar and Load once. Battery is optional.
      </p>
    {/if}

    <section class="balance-row">
      <div>
        <h3>Balance</h3>
        <p>{balance.help}{balance.available
          ? ` — ${balance.percentage.toFixed(1)}% of max reading` : ''}</p>
      </div>
      <strong>{measurement(balance.value, 'W', powerDigits, true)}</strong>
    </section>
    <label class="balance-visibility">
      <input type="checkbox" checked={draft.balance_visible === true}
        disabled={busy}
        on:change={(event) => draft = {
          ...draft, balance_visible: event.currentTarget.checked,
        }} />
      Show Balance in graphs
      <small>Usage and Power graphs</small>
    </label>

    <div class="mapping-actions">
      <button type="button" class="secondary" disabled={busy} on:click={cancel}>
        Cancel
      </button>
      <button type="button" class="primary"
        disabled={!valid || !dirty || busy} on:click={save}>
        {busy ? 'Applying…' : 'Save & Reboot'}
      </button>
    </div>
  {/if}
</article>

<style>
  .mapping-editor {
    min-width: 0;
    padding: 0;
  }

  header,
  .header-actions,
  .balance-row,
  .mapping-actions {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.75rem;
  }

  h2, h3, p { margin: 0; }
  button,
  select { font: inherit; }
  button {
    cursor: pointer;
    white-space: nowrap;
  }
  button:disabled {
    opacity: 0.55;
    cursor: default;
  }
  button.secondary {
    min-height: 2.5rem;
    padding: 0.45rem 0.75rem;
    border: 1px solid var(--border);
    border-radius: 0.3rem;
    background: transparent;
    color: var(--text);
  }
  button.primary {
    min-height: 2.5rem;
    padding: 0.45rem 0.75rem;
    border: 1px solid var(--accent);
    border-radius: 0.3rem;
    background: var(--accent);
    color: white;
  }
  header p,
  .balance-row p {
    color: var(--muted);
    font-size: 0.8rem;
  }

  .live-indicator {
    color: var(--muted);
    font-size: 0.75rem;
  }

  .live-indicator.active { color: var(--charge); }

  .close-button {
    width: 2rem;
    height: 2rem;
    padding: 0;
    border: 0;
    background: transparent;
    color: var(--muted);
    font-size: 1.35rem;
  }

  .close-button:hover:not(:disabled),
  .close-button:focus-visible { color: var(--text); }

  .sensor-list {
    --sensor-columns:
      minmax(4.6rem, 0.65fr)
      minmax(6.5rem, 1fr)
      minmax(7.2rem, 0.75fr);
    display: grid;
    gap: 0.55rem;
    margin-top: 0.9rem;
  }

  .sensor-column-headings,
  .sensor-card {
    display: grid;
    grid-template-columns: var(--sensor-columns);
    gap: 0.3rem 0.75rem;
    min-width: 0;
  }

  .sensor-column-headings {
    padding: 0 0.8rem;
    color: var(--muted);
    font-size: 0.72rem;
  }

  .sensor-column-headings span {
    min-width: 0;
    white-space: nowrap;
  }

  .sensor-card {
    align-items: center;
    padding: 0.8rem;
    border: 1px solid var(--border);
    border-radius: 0.45rem;
    background: var(--surface);
  }

  .sensor-card h3,
  .balance-row h3 {
    font-size: 0.95rem;
  }

  .sensor-card-heading {
    min-width: 0;
  }

  .sensor-card-heading h3 {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .sensor-state {
    display: grid;
    width: 1.4rem;
    height: 1.4rem;
    place-items: center;
    border-radius: 50%;
    font-weight: 700;
  }

  .sensor-state.good { color: var(--charge); }
  .sensor-state.warning,
  .interpretation.warning,
  .mapping-validation,
  .mapping-error { color: var(--battery); }
  .sensor-state.muted { color: var(--muted); }

  .sensor-controls {
    display: contents;
  }

  .visually-hidden {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
    white-space: nowrap;
    border: 0;
  }

  select {
    width: 100%;
    min-height: 2.5rem;
    padding: 0.4rem 0.55rem;
    border: 1px solid var(--border);
    border-radius: 0.35rem;
    background: var(--background);
    color: var(--text);
  }

  fieldset {
    min-width: 0;
    margin: 0;
    padding: 0;
    border: 0;
  }

  .direction-segments {
    display: grid;
    grid-template-columns: 1fr 1fr;
  }

  .direction-segments button {
    min-height: 2.5rem;
    border: 1px solid var(--border);
    background: var(--background);
    color: var(--text);
    font-size: 1.1rem;
  }

  .direction-segments button:first-child {
    border-radius: 0.35rem 0 0 0.35rem;
  }

  .direction-segments button:last-child {
    margin-left: -1px;
    border-radius: 0 0.35rem 0.35rem 0;
  }

  .direction-segments button.active {
    position: relative;
    border-color: var(--accent);
    background: var(--accent);
    color: white;
  }

  .sensor-readings {
    display: grid;
    grid-column: 1 / -1;
    grid-template-columns: 1.4rem repeat(3, minmax(0, 1fr));
    align-items: center;
    gap: 0.25rem;
    margin-top: 0.75rem;
    font-variant-numeric: tabular-nums;
    font-size: 1.05rem;
    font-weight: 600;
  }

  .sensor-readings span { white-space: nowrap; }
  .sensor-readings span:not(.sensor-state) { text-align: right; }

  .editable-reading {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 0.25rem;
  }

  .edit-reading {
    width: 1.5rem;
    height: 1.5rem;
    padding: 0;
    border: 0;
    background: transparent;
    color: var(--muted);
    font-size: 0.95rem;
  }

  .edit-reading:hover:not(:disabled),
  .edit-reading:focus-visible { color: var(--text); }

  .interpretation {
    grid-column: 1 / -1;
    min-height: 1rem;
    margin-top: 0.3rem;
    color: var(--muted);
    font-size: 0.78rem;
  }

  .mapping-validation,
  .mapping-error {
    margin: 0.75rem 0 0;
    font-size: 0.82rem;
  }

  .balance-row {
    align-items: flex-start;
    margin-top: 0.85rem;
    padding-top: 0.8rem;
    border-top: 1px solid var(--border);
  }

  .balance-row strong {
    white-space: nowrap;
    font-variant-numeric: tabular-nums;
    font-size: 1.35rem;
  }

  .balance-visibility {
    display: grid;
    grid-template-columns: auto 1fr;
    align-items: center;
    gap: 0.15rem 0.45rem;
    margin-top: 0.75rem;
    font-size: 0.88rem;
  }

  .balance-visibility input {
    grid-row: 1 / 3;
    accent-color: var(--accent);
  }

  .balance-visibility small {
    color: var(--muted);
    font-size: 0.72rem;
  }

  .mapping-actions {
    justify-content: flex-end;
    margin-top: 0.55rem;
  }

  .mapping-actions button { min-width: 8.5rem; }

  .empty {
    margin: 1rem 0;
    color: var(--muted);
  }

  @media (max-width: 34rem) {
    .sensor-list {
      --sensor-columns:
        minmax(4.3rem, 0.6fr)
        minmax(5.8rem, 1fr)
        minmax(6.5rem, 0.75fr);
    }
    .sensor-column-headings {
      padding: 0 0.65rem;
      font-size: 0.68rem;
    }
    .sensor-card {
      column-gap: 0.45rem;
      padding: 0.65rem;
    }
    .sensor-readings { font-size: 0.95rem; }
    .mapping-actions {
      display: grid;
      grid-template-columns: 1fr 1fr;
    }
    .mapping-actions button { min-width: 0; }
  }
</style>
