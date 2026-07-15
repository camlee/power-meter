<script>
  import { onMount } from 'svelte';
  import LiveChart from './lib/LiveChart.svelte';
  import HistoryChart from './lib/HistoryChart.svelte';
  import {
    anchorTime,
    getHistory,
    getRemoteScreenshot,
    getSensors,
    getStatus,
    openLiveSocket,
    sendRemotePointer,
  } from './lib/api.js';

  // ---------------------------------------------------------------------
  // Constants
  // ---------------------------------------------------------------------

  // How often to poll the REST status endpoint.
  const STATUS_POLL_MS = 10_000;
  const SENSOR_POLL_MS = 1_000;

  // Cap on how many live-chart points we keep in memory.
  const MAX_LIVE_POINTS = 600;

  // Remote display resolution, used to translate pointer events into
  // on-device coordinates.
  const REMOTE_DISPLAY_WIDTH = 320;
  const REMOTE_DISPLAY_HEIGHT = 480;

  // Throttle for pointer-drag events and debounce for the resulting
  // screenshot refresh, so we don't flood the meter with requests.
  const POINTER_MOVE_THROTTLE_MS = 80;
  const REMOTE_REFRESH_DEBOUNCE_MS = 300;

  // Live-socket reconnect backoff. Exponential with jitter, capped, so a
  // flaky connection can't hammer the meter in a tight reconnect loop.
  const RECONNECT_BASE_MS = 1_000;
  const RECONNECT_MAX_MS = 30_000;
  const RECONNECT_WARN_AFTER_ATTEMPTS = 6;

  // History ranges. `minutes` controls the server-side bucket size;
  // `refreshMs` controls how often we auto-refresh this view while it's
  // open (null = no periodic refresh, manual only).
  const historyRanges = [
    { id: 'last1hour', label: 'Last 1 Hour', minutes: 2, refreshMs: 60_000 },
    { id: 'last6hours', label: 'Last 6 Hours', minutes: 15, refreshMs: 5 * 60_000 },
    { id: 'last24hours', label: 'Last 24 Hours', minutes: 30, refreshMs: 5 * 60_000 },
    { id: 'today', label: 'Today', minutes: 30, refreshMs: 5 * 60_000 },
    { id: 'yesterday', label: 'Yesterday', minutes: 30, refreshMs: null },
    { id: 'last2days', label: 'Last 2 Days', minutes: 60, refreshMs: 5 * 60_000 },
    { id: 'lastweek', label: 'Last Week', minutes: 240, refreshMs: 15 * 60_000 },
    { id: 'all', label: 'All', minutes: 0, refreshMs: 15 * 60_000 },
  ];

  // Route <-> path mapping. Declared here (not down in the "Routing"
  // section) because `route` below initializes eagerly by calling
  // routeFromPath() at component construction time — if this constant
  // were declared later in the file, that call would hit it before its
  // `const` had run and throw a temporal-dead-zone ReferenceError.
  const ROUTE_PATHS = {
    overview: '/',
    remote: '/remote',
    history: '/history',
    sensors: '/sensors',
    setup: '/setup',
  };

  // ---------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------

  let route = routeFromPath();

  // Status polling (REST).
  let status = null;
  let statusError = '';
  let statusPollTimer;

  // Detailed sensor read model. This is polled separately from operational
  // status because it includes raw out-of-range values and UART diagnostics.
  let sensorStatus = null;
  let sensorStatusError = '';
  let sensorPollTimer;

  // Live socket (WebSocket).
  let socket = null;
  let connection = 'connecting'; // connecting | live | limited | offline | paused
  let connectionLimit = 5;
  let connectionError = '';
  let reconnectAttempts = 0;
  let reconnectTimer;
  let livePaused = false;
  let destroyed = false;

  // Live chart data.
  let points = [];
  let lastRevision = 0;
  let lastSessionId = null;
  let lastSequence = 0;

  // History view.
  let history = null;
  let historyError = '';
  let historyBusy = false;
  let historyRange = historyRanges[0];
  let historyFetchedAt = 0;
  let historyRefreshTimer;

  // Remote view.
  let remoteImage = '';
  let remoteError = '';
  let remoteBusy = false;
  let remoteRefreshTimer;
  let lastPointerAt = 0;

  // ---------------------------------------------------------------------
  // Small helpers
  // ---------------------------------------------------------------------

  const meterName = () => status?.hostname || 'meter';

  function connectionLabel() {
    switch (connection) {
      case 'live':
        return 'Live connection';
      case 'limited':
        return `Connection limit reached (${connectionLimit})`;
      case 'offline':
        return 'Connection offline — reconnecting…';
      case 'paused':
        return 'Connection paused (tab in background)';
      default:
        return 'Connecting…';
    }
  }

  function formatUptime(milliseconds) {
    if (!Number.isFinite(milliseconds)) return '—';
    const seconds = Math.floor(milliseconds / 1000);
    const hours = Math.floor(seconds / 3600);
    const minutes = String(Math.floor(seconds / 60) % 60).padStart(2, '0');
    const secs = String(seconds % 60).padStart(2, '0');
    return `${hours}h ${minutes}m ${secs}s`;
  }

  function formatMeasurement(value, unit, digits = 2) {
    return Number.isFinite(value) ? `${value.toFixed(digits)} ${unit}` : '—';
  }

  function formatPercent(value) {
    return Number.isFinite(value) ? `${(value * 100).toFixed(0)}%` : '—';
  }

  function stateLabel(state) {
    switch (state) {
      case 'valid': return 'Good';
      case 'out_of_range': return 'Out of Range';
      case 'not_configured': return 'Not configured';
      case 'waiting': return 'Waiting for data';
      case 'invalid': return 'Invalid data';
      case 'stale': return 'Stale';
      default: return 'Unknown';
    }
  }

  function sourceStateLabel(source) {
    if (!source) return 'Unknown';
    if (!source.transport) return 'Active';
    switch (source.transport.state) {
      case 'receiving': return 'Receiving';
      case 'stale': return 'Stale';
      case 'waiting': return 'Waiting for data';
      default: return 'Unknown';
    }
  }

  // Turns a thrown error into a stable, user-facing message so different
  // failures (network down, server error, bad response) are distinguishable
  // instead of all collapsing into "undefined" or a raw stack trace.
  function describeError(err, action) {
    if (!err) return `Unable to ${action}.`;
    if (err.name === 'TypeError' && /fetch/i.test(err.message || '')) {
      return `Unable to ${action}: can't reach the meter on the network.`;
    }
    if (err.status) {
      return `Unable to ${action}: meter responded with an error (${err.status}).`;
    }
    return `Unable to ${action}: ${err.message || 'unknown error'}.`;
  }

  // ---------------------------------------------------------------------
  // Routing
  // ---------------------------------------------------------------------

  function routeFromPath() {
    if (location.pathname === '/settings') return 'setup';
    const match = Object.entries(ROUTE_PATHS).find(([, path]) => path === location.pathname);
    return match ? match[0] : 'overview';
  }

  function routePath(next) {
    return ROUTE_PATHS[next] ?? '/';
  }

  function enterRoute(next) {
    route = next;
    clearTimeout(historyRefreshTimer);

    if (next === 'history') {
      if (!history) {
        refreshHistory();
      } else {
        // Coming back to a route we already have data for — check whether
        // that data is stale enough to warrant an immediate refresh.
        scheduleHistoryRefresh();
      }
    }

    if (next === 'remote') {
      refreshRemote();
    }

    if (next === 'sensors' || next === 'setup') {
      refreshSensors();
      scheduleSensorRefresh();
    } else {
      clearInterval(sensorPollTimer);
    }
  }

  function navigate(next) {
    if (route === next) return;
    window.history.pushState({}, '', routePath(next));
    enterRoute(next);
  }

  function handlePopState() {
    enterRoute(routeFromPath());
  }

  // ---------------------------------------------------------------------
  // Status polling
  // ---------------------------------------------------------------------

  async function refreshStatus() {
    try {
      status = await getStatus();
      if (Number.isFinite(status?.ws_connection_limit)) {
        connectionLimit = status.ws_connection_limit;
      }
      statusError = '';
    } catch (err) {
      statusError = describeError(err, 'reach the meter');
    }
  }

  async function refreshSensors() {
    try {
      sensorStatus = await getSensors();
      sensorStatusError = '';
    } catch (err) {
      sensorStatusError = describeError(err, 'load sensor details');
    }
  }

  function scheduleSensorRefresh() {
    clearInterval(sensorPollTimer);
    sensorPollTimer = setInterval(refreshSensors, SENSOR_POLL_MS);
  }

  // ---------------------------------------------------------------------
  // Live socket
  // ---------------------------------------------------------------------

  function reconnectDelay(attempt) {
    const capped = Math.min(RECONNECT_MAX_MS, RECONNECT_BASE_MS * 2 ** attempt);
    // Jitter avoids every open tab retrying in lockstep.
    return capped / 2 + Math.random() * (capped / 2);
  }

  function handleFrame(frame) {
    // The ESP32 replays its last 30 seconds on every connection. A stable
    // per-boot session ID lets us reject that replay precisely while still
    // recognizing a real reboot, where both uptime and sequence restart.
    if (frame.sessionId !== lastSessionId) {
      lastSessionId = frame.sessionId;
      lastSequence = 0;
      points = [];
    }

    if (frame.sequence <= lastSequence) return; // duplicate or out-of-order frame
    lastSequence = frame.sequence;

    points = [
      ...points,
      {
        in: frame.in.power,
        out: frame.out.power,
        timestamp: Number.isFinite(frame.unixMs) ? frame.unixMs : frame.uptimeMs,
      },
    ].slice(-MAX_LIVE_POINTS);

    if (frame.stateRevision !== lastRevision) {
      lastRevision = frame.stateRevision;
      refreshStatus();
    }
  }

  function handleConnectionState(state, limit) {
    // close/error callbacks can arrive after an intentional pause or after
    // teardown. They must not overwrite "paused" or schedule a new socket.
    if (livePaused || destroyed) return;

    connection = state;
    if (limit) connectionLimit = limit;

    if (state === 'live' || state === 'limited') {
      reconnectAttempts = 0;
      connectionError = '';
      return;
    }

    if (state === 'offline') {
      // If the tab is hidden, don't burn reconnect attempts in the
      // background — we resume cleanly when it becomes visible again.
      if (document.hidden) return;

      reconnectAttempts += 1;
      connectionError =
        reconnectAttempts >= RECONNECT_WARN_AFTER_ATTEMPTS
          ? 'Having trouble maintaining a stable connection to the meter. Still retrying…'
          : '';

      clearTimeout(reconnectTimer);
      reconnectTimer = setTimeout(connectLive, reconnectDelay(reconnectAttempts));
    }
  }

  function connectLive() {
    clearTimeout(reconnectTimer);
    socket?.close();
    socket = openLiveSocket(handleFrame, handleConnectionState);
  }

  // Cleanly stop the socket and any pending reconnect while the tab is
  // hidden, instead of letting it reconnect (and potentially loop) in
  // the background.
  function pauseLive() {
    clearTimeout(reconnectTimer);
    livePaused = true;
    socket?.close();
    socket = null;
    connection = 'paused';
  }

  function resumeLive() {
    livePaused = false;
    reconnectAttempts = 0;
    connectionError = '';
    connectLive();
  }

  // ---------------------------------------------------------------------
  // History
  // ---------------------------------------------------------------------

  async function refreshHistory(range = historyRange, { silent = false } = {}) {
    historyRange = range;
    clearTimeout(historyRefreshTimer);
    if (!silent) historyBusy = true;

    try {
      history = await getHistory(range.id, range.minutes);
      historyFetchedAt = Date.now();
      historyError = '';
      historyBusy = false;
      scheduleHistoryRefresh();
    } catch (err) {
      historyError = describeError(err, 'load usage history');
      historyBusy = false;
      // Retry sooner than the normal cadence after a failure, so a
      // transient blip recovers quickly instead of waiting minutes.
      const retryDelay = Math.min(range.refreshMs || 60_000, 30_000);
      historyRefreshTimer = setTimeout(() => refreshHistory(range, { silent: true }), retryDelay);
    }
  }

  // Schedules the next automatic history refresh for the current range,
  // based on how long it's actually been since the last successful fetch.
  // If the tab was backgrounded for hours, this fires (almost) immediately
  // instead of waiting out the full interval.
  function scheduleHistoryRefresh() {
    clearTimeout(historyRefreshTimer);
    if (route !== 'history') return;

    const interval = historyRange.refreshMs;
    if (!interval) return; // this range is manual-refresh only

    const elapsed = Date.now() - historyFetchedAt;
    const delay = Math.max(0, interval - elapsed);
    historyRefreshTimer = setTimeout(() => refreshHistory(historyRange, { silent: true }), delay);
  }

  function selectHistory(event) {
    const range = historyRanges.find((item) => item.id === event.currentTarget.value);
    if (range) refreshHistory(range);
  }

  // ---------------------------------------------------------------------
  // Remote display
  // ---------------------------------------------------------------------

  async function loadRemote() {
    try {
      const next = await getRemoteScreenshot();
      if (remoteImage) URL.revokeObjectURL(remoteImage);
      remoteImage = next;
      remoteError = '';
    } catch (err) {
      remoteError = describeError(err, 'load the remote display');
    }
  }

  async function refreshRemote() {
    if (remoteBusy) return;
    remoteBusy = true;
    try {
      await loadRemote();
    } finally {
      remoteBusy = false;
    }
  }

  function scheduleRemoteRefresh() {
    clearTimeout(remoteRefreshTimer);
    remoteBusy = true;
    remoteRefreshTimer = setTimeout(async () => {
      try {
        await loadRemote();
      } finally {
        remoteBusy = false;
      }
    }, REMOTE_REFRESH_DEBOUNCE_MS);
  }

  function remoteCoordinates(event) {
    const bounds = event.currentTarget.getBoundingClientRect();
    return {
      x: Math.max(0, Math.min(
        REMOTE_DISPLAY_WIDTH - 1,
        Math.round((event.clientX - bounds.left) * REMOTE_DISPLAY_WIDTH / bounds.width),
      )),
      y: Math.max(0, Math.min(
        REMOTE_DISPLAY_HEIGHT - 1,
        Math.round((event.clientY - bounds.top) * REMOTE_DISPLAY_HEIGHT / bounds.height),
      )),
    };
  }

  async function remotePoint(event, pressed) {
    try {
      const { x, y } = remoteCoordinates(event);
      await sendRemotePointer(x, y, pressed);
      if (!pressed) scheduleRemoteRefresh();
    } catch (err) {
      remoteError = describeError(err, 'control the remote display');
    }
  }

  function startRemotePointer(event) {
    event.currentTarget.setPointerCapture(event.pointerId);
    remotePoint(event, true);
  }

  function moveRemotePointer(event) {
    if (!event.buttons) return;
    if (Date.now() - lastPointerAt < POINTER_MOVE_THROTTLE_MS) return;
    lastPointerAt = Date.now();
    remotePoint(event, true);
  }

  function finishRemotePointer(event) {
    remotePoint(event, false);
  }

  // ---------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------

  function handleVisibilityChange() {
    if (document.hidden) {
      pauseLive();
      // Don't keep polling history/remote in the background either.
      clearTimeout(historyRefreshTimer);
      clearInterval(sensorPollTimer);
      return;
    }

    // Tab is visible again: resync the clock, reconnect live data, and
    // refresh whatever view is open if its data has gone stale.
    anchorTime();
    resumeLive();
    refreshStatus();

    if (route === 'history') scheduleHistoryRefresh();
    if (route === 'remote') refreshRemote();
    if (route === 'sensors' || route === 'setup') {
      refreshSensors();
      scheduleSensorRefresh();
    }
  }

  onMount(() => {
    destroyed = false;
    refreshStatus();
    anchorTime();
    connectLive();
    enterRoute(route);

    statusPollTimer = setInterval(refreshStatus, STATUS_POLL_MS);

    document.addEventListener('visibilitychange', handleVisibilityChange);
    window.addEventListener('popstate', handlePopState);

    return () => {
      destroyed = true;
      clearInterval(statusPollTimer);
      clearInterval(sensorPollTimer);
      clearTimeout(reconnectTimer);
      clearTimeout(remoteRefreshTimer);
      clearTimeout(historyRefreshTimer);
      socket?.close();
      if (remoteImage) URL.revokeObjectURL(remoteImage);
      document.removeEventListener('visibilitychange', handleVisibilityChange);
      window.removeEventListener('popstate', handlePopState);
    };
  });
</script>

<main>
  <header class="appbar">
    <h1>{meterName()}</h1>
    <span
      class="connection"
      class:live={connection === 'live'}
      class:offline={connection === 'offline' || connection === 'limited'}
      class:paused={connection === 'paused'}
      aria-label={connectionLabel()}
      title={connectionLabel()}
    >
      <b></b>
    </span>
    {#if connection === 'limited'}
      <span class="connection-message">Connection limit reached ({connectionLimit})</span>
    {/if}
  </header>

  <nav aria-label="Main navigation">
    <button class:active={route === 'overview'} on:click={() => navigate('overview')}>Power</button>
    <button class:active={route === 'history'} on:click={() => navigate('history')}>Usage</button>
    <button class:active={route === 'sensors'} on:click={() => navigate('sensors')}>Sensors</button>
    <button class:active={route === 'setup'} on:click={() => navigate('setup')}>Setup</button>
    <button class:active={route === 'remote'} on:click={() => navigate('remote')}>Remote</button>
  </nav>

  {#if statusError}<p class="error" role="alert">{statusError}</p>{/if}
  {#if connectionError}<p class="error" role="alert">{connectionError}</p>{/if}

  {#if route === 'remote'}
    <section class="remote-view">
      {#if remoteError}<p class="error" role="alert">{remoteError}</p>{/if}
      {#if remoteImage}
        <img
          class="remote"
          src={remoteImage}
          alt="Meter display"
          on:pointerdown={startRemotePointer}
          on:pointermove={moveRemotePointer}
          on:pointerup={finishRemotePointer}
          on:pointercancel={finishRemotePointer}
        />
      {:else}
        <p class="empty">{remoteBusy ? 'Loading display…' : 'Display unavailable.'}</p>
      {/if}
      {#if remoteBusy}
        <span class="progress remote-progress" role="status" aria-label="Refreshing display"></span>
      {/if}
      <button class="refresh" on:click={refreshRemote} disabled={remoteBusy} aria-label="Refresh display" title="Refresh display">
        ↻
      </button>
    </section>
  {:else if route === 'setup'}
    <section class="setup-view">
      {#if sensorStatusError}<p class="error" role="alert">{sensorStatusError}</p>{/if}
      <h2>Sensor source</h2>
      <dl class="details">
        <dt>Active source</dt><dd>{sensorStatus?.source?.label || '—'}</dd>
        <dt>Source status</dt>
        <dd>
          <span class="state" class:good={sensorStatus?.source?.transport?.state === 'receiving' || (sensorStatus?.source && !sensorStatus.source.transport)} class:warning={sensorStatus?.source?.transport?.state === 'stale'}>
            {sourceStateLabel(sensorStatus?.source)}
          </span>
        </dd>
      </dl>

      <div class="setup-channels" aria-label="Sensor channel status">
        {#each sensorStatus?.channels || [] as channel}
          <span class="channel-summary" class:good={channel.state === 'valid'} class:warning={channel.state === 'out_of_range'} class:bad={channel.state === 'invalid' || channel.state === 'stale'}>
            <b>{channel.label}</b> {stateLabel(channel.state)}
          </span>
        {/each}
      </div>

      {#if sensorStatus?.source?.transport}
        <h2>UART diagnostics</h2>
        <dl class="details diagnostics">
          <dt>Connection</dt><dd>{sensorStatus.source.transport.connected ? 'Receiving' : 'Not receiving'}</dd>
          <dt>Last valid frame</dt><dd>{Number.isFinite(sensorStatus.source.transport.last_valid_age_ms) ? `${sensorStatus.source.transport.last_valid_age_ms} ms ago` : 'None received'}</dd>
          <dt>Channels present</dt><dd>{sensorStatus.channels.filter((channel) => channel.configured).map((channel) => channel.label).join(', ') || 'None'}</dd>
          <dt>Sequence</dt><dd>{sensorStatus.source.transport.sequence ?? '—'}</dd>
          <dt>Producer uptime</dt><dd>{formatUptime(sensorStatus.source.transport.source_uptime_ms)}</dd>
          <dt>Valid frames</dt><dd>{sensorStatus.source.transport.valid_frames ?? '—'}</dd>
          <dt>Invalid frames</dt><dd>{sensorStatus.source.transport.invalid_frames ?? '—'}</dd>
          <dt>Last parser error</dt><dd>{sensorStatus.source.transport.last_error || '—'}</dd>
          <dt>Checksum errors</dt><dd>{sensorStatus.source.transport.checksum_errors ?? '—'}</dd>
          <dt>Overflow frames</dt><dd>{sensorStatus.source.transport.overflow_frames ?? '—'}</dd>
          <dt>Duplicate frames</dt><dd>{sensorStatus.source.transport.duplicate_frames ?? '—'}</dd>
          <dt>Missing frames</dt><dd>{sensorStatus.source.transport.missing_frames ?? '—'}</dd>
          <dt>Sequence gaps</dt><dd>{sensorStatus.source.transport.sequence_gap_events ?? '—'}</dd>
          <dt>Sequence resets</dt><dd>{sensorStatus.source.transport.sequence_resets ?? '—'}</dd>
        </dl>
      {/if}

      <h2>Device</h2>
      <dl>
        <dt>Uptime</dt><dd>{formatUptime(status?.uptime_ms)}</dd>
        <dt>Date</dt><dd>{status?.date || '—'}</dd>
        <dt>Time</dt><dd>{status?.time || '—'}</dd>
        <dt>Time source</dt><dd>{status?.time_source || 'unanchored'}</dd>
        <dt>Build</dt><dd>{status?.build_version ? `v${status.build_version}` : '—'}</dd>
        <dt>Build date</dt><dd>{status?.build_date || '—'}</dd>
        <dt>Build time</dt><dd>{status?.build_time || '—'}</dd>
        <dt>Web build</dt><dd>{status?.web_build || '—'}</dd>
        <dt>Data storage</dt><dd>{Number.isFinite(status?.data_storage_percent) ? `${status.data_storage_percent}%` : '—'}</dd>
        <dt>Station IP</dt><dd>{status?.network?.station_ip || '—'}</dd>
        <dt>AP IP</dt><dd>{status?.network?.ap_ip || '—'}</dd>
        <dt>WS connections</dt><dd>{status?.ws_connections ?? '—'} / {status?.ws_connection_limit ?? '—'}</dd>
      </dl>
    </section>
  {:else if route === 'sensors'}
    <section class="sensors-view" aria-live="polite">
      {#if sensorStatusError}<p class="error" role="alert">{sensorStatusError}</p>{/if}
      <div class="sensor-heading">
        <div>
          <h2>Live sensors</h2>
          <p>{sensorStatus?.source?.label || '—'} source · {sourceStateLabel(sensorStatus?.source)}</p>
        </div>
        <button class="refresh" on:click={refreshSensors} aria-label="Refresh sensors" title="Refresh sensors">↻</button>
      </div>
      <div class="sensor-grid">
        {#each sensorStatus?.channels || [] as channel}
          <article class="sensor-card" class:warning={channel.state === 'out_of_range'} class:bad={channel.state === 'invalid' || channel.state === 'stale'}>
            <header>
              <h3>{channel.label}</h3>
              <span class="state" class:good={channel.state === 'valid'} class:warning={channel.state === 'out_of_range'} class:bad={channel.state === 'invalid' || channel.state === 'stale'}>{stateLabel(channel.state)}</span>
            </header>
            <dl class="measurements">
              <dt>Voltage</dt><dd>{formatMeasurement(channel.voltage, 'V')}</dd>
              <dt>Current</dt><dd>{formatMeasurement(channel.current, 'A')}</dd>
              <dt>Power</dt><dd>{formatMeasurement(channel.power, 'W')}</dd>
              <dt>Duty</dt><dd>{channel.duty?.state === 'valid' ? formatPercent(channel.duty.value) : '—'}</dd>
            </dl>
            {#if !channel.configured}<p class="sensor-note">This channel is not configured by the active source.</p>{/if}
          </article>
        {/each}
      </div>
      {#if !sensorStatus && !sensorStatusError}<p class="empty">Loading sensors…</p>{/if}
    </section>
  {:else if route === 'history'}
    <section class="history-view">
      <div class="history-controls">
        <label class="sr-only" for="history-range">History range</label>
        <select id="history-range" value={historyRange.id} on:change={selectHistory} disabled={historyBusy}>
          {#each historyRanges as range}
            <option value={range.id}>{range.label}</option>
          {/each}
        </select>
        <button class="refresh" on:click={() => refreshHistory()} disabled={historyBusy} aria-label="Refresh history" title="Refresh history">
          ↻
        </button>
      </div>

      {#if historyError}<p class="error" role="alert">{historyError}</p>{/if}
      {#if historyBusy}<span class="progress" role="status" aria-label="Loading history"></span>{/if}

      <p class="chart-legend">
        <span class="charge"></span>Charge
        <span class="panel-color"></span>Panel
        <span class="surplus"></span>Surplus
        <span class="battery"></span>Battery
        <span class="load"></span>Load
      </p>

      {#if history}
        <HistoryChart buckets={history.buckets} />
        {#if history.flags & 1}
          <p class="note">Some intervals have incomplete coverage.</p>
        {/if}
      {:else if !historyBusy}
        <p class="empty">No history available.</p>
      {/if}
    </section>
  {:else}
    <section class="live-view">
      <p class="chart-legend">
        <span class="charge"></span>In
        <span class="battery"></span>Out
      </p>
      <LiveChart {points} sessionId={lastSessionId} active={!livePaused} />
    </section>
  {/if}
</main>

<style>
  :global(*) {
    box-sizing: border-box;
  }

  :global(:root) {
    color-scheme: light dark;
    --background: #fff;
    --text: #202428;
    --muted: #68747c;
    --border: #d8dde0;
    --accent: #1688e8;
    --charge: #159947;
    --panel: #1464df;
    --surplus: #00a6dc;
    --battery: #e44331;
    --load: #e59400;
    --warning: #b86f00;
    --surface: #f6f8f9;
    --chart-grid: #e4e8ea;
    --chart-zero: #95a1a8;
  }

  @media (prefers-color-scheme: dark) {
    :global(:root) {
      color-scheme: dark;
      --background: #101417;
      --text: #f1f5f7;
      --muted: #aab5bc;
      --border: #3c4851;
      --accent: #4ca9f5;
      --charge: #3ca76c;
      --panel: #5596e6;
      --surplus: #4db6d0;
      --battery: #e56c63;
      --load: #d99a58;
      --warning: #e0a447;
      --surface: #182025;
      --chart-grid: #344049;
      --chart-zero: #72818a;
    }
  }

  :global(body) {
    margin: 0;
    background: var(--background);
    color: var(--text);
    font: 16px/1.4 system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  }

  main {
    width: min(100%, 80rem);
    min-height: 100dvh;
    margin: auto;
    padding: 0.5rem 0.75rem;
  }

  h1, p {
    margin: 0;
  }

  h1 {
    font-size: 1.05rem;
    font-weight: 650;
    letter-spacing: -0.015em;
  }

  .appbar {
    height: 2.15rem;
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .connection b {
    display: block;
    width: 0.58rem;
    height: 0.58rem;
    border-radius: 50%;
    background: var(--load);
  }

  .connection.live b {
    background: var(--charge);
  }

  .connection.offline b {
    background: var(--battery);
  }

  .connection.paused b {
    background: var(--muted);
  }

  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
  }

  nav {
    display: flex;
    overflow-x: auto;
    border-bottom: 1px solid var(--border);
    margin: 0 -0.75rem;
    padding: 0 0.75rem;
    gap: 0.25rem;
  }

  button {
    border: 1px solid transparent;
    background: transparent;
    color: var(--muted);
    font: 600 0.86rem inherit;
    cursor: pointer;
    white-space: nowrap;
  }

  button:disabled {
    opacity: 0.55;
    cursor: wait;
  }

  nav button {
    padding: 0.55rem 0.68rem;
  }

  nav button.active {
    color: var(--accent);
    box-shadow: inset 0 -2px var(--accent);
  }

  .connection-message {
    color: var(--battery);
    font-size: 0.78rem;
  }

  .error {
    color: var(--battery);
    font-size: 0.85rem;
    margin: 0.65rem 0;
  }

  .live-view,
  .history-view,
  .remote-view,
  .setup-view,
  .sensors-view {
    margin-top: 0.55rem;
  }

  .live-view {
    min-height: calc(100dvh - 6.4rem);
    display: flex;
    flex-direction: column;
  }

  .chart-legend {
    color: var(--muted);
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.28rem 0.6rem;
    margin: 0 0 0.25rem;
    font-size: 0.76rem;
  }

  .chart-legend span {
    width: 0.75rem;
    height: 0.22rem;
    border-radius: 1px;
    background: var(--charge);
  }

  .chart-legend .panel-color { background: var(--panel); }
  .chart-legend .surplus { background: var(--surplus); }
  .chart-legend .battery { background: var(--battery); }
  .chart-legend .load { background: var(--load); }

  .history-controls {
    display: flex;
    align-items: center;
    gap: 0.35rem;
  }

  .history-controls select {
    min-width: 0;
    flex: 1;
    border: 1px solid var(--border);
    border-radius: 0;
    padding: 0.38rem 2rem 0.38rem 0.55rem;
    background: var(--background);
    color: var(--text);
    font: 600 0.86rem inherit;
  }

  .progress {
    display: block;
    width: 100%;
    height: 0.22rem;
    margin: 0.45rem 0;
    overflow: hidden;
    background: var(--border);
  }

  .progress::before {
    content: '';
    display: block;
    width: 35%;
    height: 100%;
    background: var(--accent);
    animation: indeterminate 1s ease-in-out infinite;
  }

  .refresh {
    width: 2rem;
    height: 2rem;
    padding: 0;
    border: 1px solid var(--border);
    color: var(--text);
    font-size: 1.2rem;
    line-height: 1;
  }

  .empty {
    color: var(--muted);
    margin: 1.5rem 0;
    text-align: center;
  }

  .note {
    color: var(--muted);
    font-size: 0.82rem;
    margin-top: 0.6rem;
  }

  .remote-view {
    position: relative;
    display: flex;
    min-height: calc(100dvh - 6.4rem);
    align-items: center;
    justify-content: center;
  }

  .remote {
    display: block;
    width: auto;
    height: min(calc(100dvh - 7rem), 480px);
    max-width: 100%;
    object-fit: contain;
    touch-action: none;
    cursor: pointer;
    background: #111;
    image-rendering: auto;
  }

  .remote:active {
    cursor: grabbing;
  }

  .remote-view .refresh {
    position: absolute;
    top: 0;
    right: 0;
    background: var(--background);
  }

  .remote-progress {
    position: absolute;
    top: 0;
    left: 0;
    right: 2.35rem;
    width: auto;
  }

  .setup-view {
    max-width: 36rem;
  }

  h2 {
    margin: 1rem 0 0.55rem;
    font-size: 1rem;
  }

  h3 {
    margin: 0;
    font-size: 1rem;
  }

  .setup-view > h2:first-of-type {
    margin-top: 0;
  }

  .details {
    padding-bottom: 0.8rem;
    border-bottom: 1px solid var(--border);
  }

  .setup-channels {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
    margin: 0.75rem 0;
  }

  .channel-summary,
  .state {
    display: inline-block;
    color: var(--muted);
    font-size: 0.78rem;
  }

  .channel-summary {
    padding: 0.25rem 0.45rem;
    background: var(--surface);
    border-left: 3px solid var(--border);
  }

  .state.good,
  .channel-summary.good { color: var(--charge); }
  .channel-summary.good { border-left-color: var(--charge); }
  .state.warning,
  .channel-summary.warning { color: var(--warning); }
  .channel-summary.warning { border-left-color: var(--warning); }
  .state.bad,
  .channel-summary.bad { color: var(--battery); }
  .channel-summary.bad { border-left-color: var(--battery); }

  .sensor-heading {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    margin-bottom: 0.7rem;
  }

  .sensor-heading h2 { margin: 0; }
  .sensor-heading p {
    color: var(--muted);
    font-size: 0.8rem;
  }

  .sensor-grid {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 0.7rem;
  }

  .sensor-card {
    padding: 0.75rem;
    border: 1px solid var(--border);
    background: var(--surface);
  }

  .sensor-card.warning { border-color: var(--warning); }
  .sensor-card.bad { border-color: var(--battery); }

  .sensor-card header {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 0.6rem;
    margin-bottom: 0.65rem;
  }

  .measurements {
    grid-template-columns: max-content minmax(0, 1fr);
    gap: 0.35rem 0.7rem;
  }

  .measurements dd {
    text-align: right;
    font-variant-numeric: tabular-nums;
  }

  .sensor-note {
    color: var(--muted);
    margin-top: 0.7rem;
    font-size: 0.78rem;
  }

  dl {
    display: grid;
    grid-template-columns: max-content minmax(0, 1fr);
    gap: 0.55rem 1rem;
    margin: 0;
  }

  dt { color: var(--muted); }
  dd { margin: 0; overflow-wrap: anywhere; }

  @keyframes indeterminate {
    from { transform: translateX(-110%); }
    to { transform: translateX(320%); }
  }

  @media (max-width: 34rem) {
    main {
      padding-left: 0.5rem;
      padding-right: 0.5rem;
    }

    nav {
      margin-left: -0.5rem;
      margin-right: -0.5rem;
      padding-left: 0.5rem;
      padding-right: 0.5rem;
    }

    .live-view,
    .remote-view {
      min-height: calc(100dvh - 6.1rem);
    }

    .remote {
      height: min(calc(100dvh - 6.7rem), 480px);
    }

    .sensor-grid {
      grid-template-columns: 1fr;
    }
  }
</style>
