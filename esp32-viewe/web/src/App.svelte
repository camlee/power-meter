<script>
  import { onMount } from 'svelte';
  import LiveChart from './lib/LiveChart.svelte';
  import SensorChart from './lib/SensorChart.svelte';
  import AdcCaptureChart from './lib/AdcCaptureChart.svelte';
  import HistoryChart from './lib/HistoryChart.svelte';
  import {
    anchorTime,
    getAdcCaptureData,
    getAdcCaptureStatus,
    getDebug,
    getCycles,
    getHistory,
    getRemoteScreenshot,
    getSensors,
    getSetup,
    getStatus,
    getUpdates,
    getWifi,
    checkForUpdates,
    installUpdate,
    openLiveSocket,
    requestAdcCapture,
    saveWifiAp,
    sendRemotePointer,
    sendWifiStationCommand,
    saveCycleEndHour,
    saveSetup,
    saveSensorCalibration,
    saveUpdateSettings,
  } from './lib/api.js';

  // ---------------------------------------------------------------------
  // Constants
  // ---------------------------------------------------------------------

  // How often to poll the REST status endpoint.
  const STATUS_POLL_MS = 10_000;
  const SENSOR_POLL_MS = 1_000;
  const DEBUG_POLL_MS = 1_000;
  const WIFI_POLL_MS = 1_000;
  const UPDATE_POLL_MS = 1_000;
  const CYCLE_REFRESH_MS = 5 * 60_000;
  const cycleHours = Array.from({ length: 24 }, (_, hour) => ({ hour, label: hourLabel(hour) }));

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
  const RECONNECT_FAIL_AFTER_ATTEMPTS = 6;

  // History ranges. The refresh cadence matches one x-axis bucket, so a
  // multi-day query is not repeated just because another minute elapsed.
  // The all-history bucket is selected by the firmware and learned from the
  // response. Yesterday is complete and therefore manual-refresh only.
  const rollingHistoryRanges = [
    { id: 'last1hour', label: 'Last 1 Hour', minutes: 2, tickMinutes: 15, refreshMs: 2 * 60_000 },
    { id: 'last6hours', label: 'Last 6 Hours', minutes: 15, tickMinutes: 60, refreshMs: 15 * 60_000 },
    { id: 'last24hours', label: 'Last 24 Hours', minutes: 30, tickMinutes: 180, refreshMs: 30 * 60_000 },
    { id: 'last2days', label: 'Last 2 Days', minutes: 60, tickMinutes: 360, refreshMs: 60 * 60_000 },
    { id: 'lastweek', label: 'Last Week', minutes: 240, tickMinutes: 1440, refreshMs: 240 * 60_000 },
  ];
  const clockHistoryRanges = [
    ...rollingHistoryRanges,
    { id: 'today', label: 'Today', minutes: 30, tickMinutes: 180, refreshMs: 30 * 60_000 },
    { id: 'yesterday', label: 'Yesterday', minutes: 30, tickMinutes: 180, refreshMs: null },
    { id: 'all', label: 'All History', minutes: 0, tickMinutes: 0, refreshMs: null },
  ];
  const relativeHistoryRanges = [
    ...rollingHistoryRanges,
    { id: 'sinceboot', label: 'Since Boot', minutes: 0, tickMinutes: 0, refreshMs: null },
  ];
  let historyRanges = relativeHistoryRanges;

  // Route <-> path mapping. Declared here (not down in the "Routing"
  // section) because `route` below initializes eagerly by calling
  // routeFromPath() at component construction time — if this constant
  // were declared later in the file, that call would hit it before its
  // `const` had run and throw a temporal-dead-zone ReferenceError.
  const ROUTE_PATHS = {
    overview: '/',
    remote: '/remote',
    history: '/history',
    cycle: '/cycle',
    sensors: '/sensors',
    raw: '/sensors/raw',
    wifi: '/wifi',
    setup: '/setup',
    info: '/info',
    debug: '/debug',
  };

  const SETTINGS_ROUTES = ['wifi', 'setup', 'info', 'debug', 'remote'];

  // ---------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------

  let route = routeFromPath();

  // Status polling (REST).
  let status = null;
  let statusError = '';
  let statusPollTimer;
  let statusFetchedAt = 0;
  $: hasTouchDisplay = status?.capabilities?.touch_display !== false;
  $: hasStatusDisplay = status?.capabilities?.status_display === true;
  $: webThemeOptions = hasTouchDisplay
    ? ['light', 'dark', 'auto', 'device']
    : ['light', 'dark', 'auto'];
  $: sensorModeOptions = [
    ['adc', 'ESP32 ADC'], ['ads1115', 'ADS1115'], ['uart', 'UART'], ['demo', 'Demo'],
  ].filter(([mode]) => status?.capabilities?.sensor_modes?.[mode] !== false);
  $: if (status && !hasTouchDisplay && route === 'remote') {
    window.history.replaceState({}, '', routePath('setup'));
    enterRoute('setup');
  }

  // Detailed sensor read model. This is polled separately from operational
  // status because it includes raw out-of-range values and UART diagnostics.
  let sensorStatus = null;
  let sensorStatusError = '';
  let sensorPollTimer;
  let selectedSensor = 'in';
  let calibrationEditor = null;
  let calibrationReference = '';
  let calibrationBusy = false;
  let calibrationError = '';
  let calibrationMessage = '';
  let adcCapture = null;
  let adcCaptureBusy = false;
  let adcCaptureState = '';
  let adcCaptureError = '';
  let adcCaptureSensor = 'in';
  let adcCaptureId = 0;
  let adcCapturePollGeneration = 0;
  let adcCapturePollTimer;
  $: selectedChannel = sensorStatus?.channels?.find((channel) => channel.id === selectedSensor) || null;
  $: calibrationInput = !calibrationEditor || !selectedChannel
    ? Number.NaN
    : calibrationEditor.measurement === 'voltage'
      ? selectedChannel.input_voltage_v
      : selectedChannel.input_current_v;
  $: sensorPoints = Object.fromEntries(['in', 'out', 'aux'].map((id) => [id, points.map((point) => ({
    timestamp: point.timestamp, receivedAt: point.receivedAt,
    receivedMonotonicAt: point.receivedMonotonicAt, ...(point.sensors?.[id] || {}),
  }))]));
  $: selectedSensorPoints = sensorPoints[selectedSensor] || [];
  $: calibrationChartPoints = calibrationEditor?.chartUnits === 'raw'
    ? rawCalibrationPoints(
      calibrationEditor, selectedSensor, selectedChannel, selectedSensorPoints,
    )
    : selectedSensorPoints;
  // Pass dependencies explicitly: legacy Svelte reactive statements cannot
  // discover state read indirectly inside a zero-argument helper.
  $: calibrationPreviewPoints = previewCalibrationPoints(
    calibrationEditor, selectedSensor, selectedChannel, selectedSensorPoints,
  );

  // Setup writes are staged locally and applied together by the device.
  let setup = null;
  let setupHostname = '';
  let setupSensorMode = 'adc';
  let setupAppearance = 'auto';
  let setupStatusDisplayMode = 'summary';
  let resetSetup = false;
  let resetWifi = false;
  let resetCalibration = false;
  let resetUsage = false;
  let setupError = '';
  let setupMessage = '';
  let setupBusy = false;
  $: setupIsDirty = !!setup && (
    setupHostname !== setup.hostname || setupSensorMode !== setup.sensor_mode ||
    setupAppearance !== setup.appearance ||
    setupStatusDisplayMode !== (setup.status_display_mode || 'summary') || resetSetup || resetWifi ||
    resetCalibration || resetUsage
  );

  // Wi-Fi commands are applied immediately by the shared network manager.
  // Poll while this page is visible because scans and connections finish
  // asynchronously and may also be changed from the touchscreen.
  let wifi = null;
  let wifiError = '';
  let wifiMessage = '';
  let wifiBusy = false;
  let wifiPollTimer;
  let stationSsid = '';
  let stationPassword = '';
  let stationSecure = false;
  let apDraftLoaded = false;
  let apEnabled = false;
  let apSsid = '';
  let apSecure = true;
  let apPassword = '';
  $: wifiNetworks = wifi?.scan?.networks || [];
  $: wifiScanBusy = ['starting', 'running'].includes(wifi?.scan?.state);

  // On-device diagnostics are refreshed only while Debug is visible.
  let debugStatus = null;
  let debugError = '';
  let debugPollTimer;

  let updateStatus = null;
  let updateError = '';
  let updateActionBusy = false;
  let updatePollTimer;

  // This preference controls only the browser. "auto" follows the browser/
  // OS color preference; "device" follows the display's effective theme.
  let webTheme = loadWebTheme();
  let colorSchemeMedia;

  // Live socket (WebSocket).
  let socket = null;
  let connection = 'connecting'; // connecting | reconnecting | live | failed | limited | paused
  let connectionLimit = 5;
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
  let historyRequestGeneration = 0;
  let historyRangesHaveTime = false;

  // Energy cycle view.
  let cycles = null;
  let cycleError = '';
  let cycleBusy = false;
  let cycleEndHour = 20;
  let cycleFetchedAt = 0;
  let cycleRefreshTimer;
  $: cycleSummaries = cycles?.cycles || [];
  $: displayedCycles = [...cycleSummaries].reverse();
  $: currentCycle = cycleSummaries.find((cycle) => cycle.current) || null;
  $: netWindow = cycleSummaries.slice(-7);
  $: sevenDayNet = netWindow.length === 7 && netWindow.every((cycle) => Number.isFinite(cycle.net_wh))
    ? netWindow.reduce((total, cycle) => total + cycle.net_wh, 0)
    : null;

  // Remote view.
  let remoteImage = '';
  let remoteError = '';
  let remoteBusy = false;
  let remoteRefreshTimer;
  let lastPointerAt = 0;

  // ---------------------------------------------------------------------
  // Small helpers
  // ---------------------------------------------------------------------

  function loadWebTheme() {
    try {
      const saved = localStorage.getItem('viewe-web-theme');
      return ['light', 'dark', 'auto', 'device'].includes(saved) ? saved : 'device';
    } catch (_) {
      return 'device';
    }
  }

  function applyWebTheme() {
    let resolved = webTheme;
    if (resolved === 'auto') {
      resolved = matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
    } else if (resolved === 'device') {
      resolved = hasTouchDisplay && typeof status?.appearance?.dark === 'boolean'
        ? (status.appearance.dark ? 'dark' : 'light')
        : (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
    }
    document.documentElement.dataset.theme = resolved;
    document.querySelector('meta[name="theme-color"]')?.setAttribute(
      'content', resolved === 'dark' ? '#101417' : '#ffffff',
    );
    window.dispatchEvent(new Event('viewe-theme-change'));
  }

  function selectWebTheme(next) {
    webTheme = next;
    try { localStorage.setItem('viewe-web-theme', next); } catch (_) { /* storage may be disabled */ }
    applyWebTheme();
  }

  function colorSchemeChanged() {
    if (webTheme === 'auto' ||
        (webTheme === 'device' && (!hasTouchDisplay || !status?.appearance))) applyWebTheme();
  }

  function connectionLabel() {
    switch (connection) {
      case 'live':
        return 'Live connection';
      case 'limited':
        return `Connection limit reached (${connectionLimit})`;
      case 'reconnecting':
        return 'Reconnecting…';
      case 'failed':
        return 'Failed to connect; still retrying…';
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

  function hourLabel(hour) {
    const normalized = hour % 12 || 12;
    return `${normalized}:00 ${hour < 12 ? 'AM' : 'PM'}`;
  }

  function formatCycleEnergy(value, unit = false, signed = unit) {
    return Number.isFinite(value) ? `${value >= 0 && signed ? '+' : ''}${Math.round(value)}${unit ? ' Wh' : ''}` : '—';
  }

  function cycleNetClass(value) {
    return Number.isFinite(value) ? (value > 0 ? 'positive' : value < 0 ? 'negative' : 'neutral') : 'unavailable';
  }

  function cycleDay(cycle) {
    if (cycle.current) return 'Today';
    const shifted = cycle.end_unix_ms - 1 + (cycles?.utc_offset_minutes || 0) * 60_000;
    return new Intl.DateTimeFormat(undefined, { weekday: 'short', timeZone: 'UTC' }).format(new Date(shifted));
  }

  function formatPercent(value) {
    return Number.isFinite(value) ? `${(value * 100).toFixed(0)}%` : '—';
  }

  function dutyEmptyMessage(channel) {
    if (channel?.duty?.state === 'invalid') return 'Duty reading unavailable.';
    if (channel?.duty?.state === 'valid') return 'Waiting for readings…';
    return 'Duty is not reported by this sensor.';
  }

  function titleCase(value) {
    if (!value) return '—';
    return value.split('_').map((part) => part.charAt(0).toUpperCase() + part.slice(1)).join(' ');
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

  function sensorLabel(id) {
    return sensorStatus?.channels?.find((channel) => channel.id === id)?.label ||
      ({ in: 'In', out: 'Out', aux: 'Aux' }[id] || 'Sensor');
  }

  function selectSensor(id) {
    selectedSensor = id;
    calibrationEditor = null;
    calibrationError = calibrationMessage = '';
  }

  async function pollAdcCapture(
    captureId = adcCaptureId,
    generation = adcCapturePollGeneration,
  ) {
    try {
      const captureStatus = await getAdcCaptureStatus();
      if (route !== 'raw' || adcCaptureId !== captureId ||
          adcCapturePollGeneration !== generation) return;
      if (captureStatus.capture_id !== captureId) {
        throw new Error('capture was replaced or expired');
      }
      adcCaptureState = captureStatus.state;
      adcCaptureSensor = captureStatus.channel || adcCaptureSensor;
      if (captureStatus.state === 'ready') {
        const result = await getAdcCaptureData(captureId);
        if (route !== 'raw' || adcCaptureId !== captureId ||
            adcCapturePollGeneration !== generation) return;
        adcCapture = result;
        adcCaptureSensor = adcCapture.channel;
        adcCaptureBusy = false;
        adcCaptureState = 'ready';
        return;
      }
      if (!['armed', 'capturing'].includes(captureStatus.state)) {
        throw new Error(`capture entered ${captureStatus.state} state`);
      }
      adcCapturePollTimer = setTimeout(
        () => pollAdcCapture(captureId, generation), 150,
      );
    } catch (err) {
      if (adcCapturePollGeneration !== generation) return;
      adcCaptureBusy = false;
      adcCaptureError = describeError(err, 'capture ADC readings');
    }
  }

  async function resumeAdcCapture() {
    if (adcCaptureBusy) return;
    const generation = ++adcCapturePollGeneration;
    try {
      const captureStatus = await getAdcCaptureStatus();
      if (route !== 'raw' || adcCapturePollGeneration !== generation) return;
      if (!['armed', 'capturing', 'ready'].includes(captureStatus.state)) return;
      adcCaptureBusy = true;
      adcCaptureError = '';
      adcCaptureState = captureStatus.state;
      adcCaptureSensor = captureStatus.channel || adcCaptureSensor;
      adcCaptureId = captureStatus.capture_id;
      pollAdcCapture(adcCaptureId, generation);
    } catch (_) {
      // This is opportunistic recovery after navigation or a page reload.
      // A normal idle/unavailable meter needs no user-facing message.
    }
  }

  async function startAdcCapture(channel = 'in') {
    clearTimeout(adcCapturePollTimer);
    adcCaptureSensor = channel;
    adcCaptureBusy = true;
    adcCapture = null;
    adcCaptureError = '';
    adcCaptureState = 'arming';
    adcCaptureId = 0;
    const generation = ++adcCapturePollGeneration;
    if (route !== 'raw') navigate('raw');
    try {
      const existing = await getAdcCaptureStatus();
      if (route !== 'raw' || adcCapturePollGeneration !== generation) return;
      if (['armed', 'capturing', 'ready'].includes(existing.state)) {
        if (existing.channel !== adcCaptureSensor) {
          throw new Error(
            `${sensorLabel(existing.channel)} already has an active capture`,
          );
        }
        adcCaptureState = existing.state;
        adcCaptureSensor = existing.channel || adcCaptureSensor;
        adcCaptureId = existing.capture_id;
        pollAdcCapture(adcCaptureId, generation);
        return;
      }
      const started = await requestAdcCapture(adcCaptureSensor);
      if (route !== 'raw' || adcCapturePollGeneration !== generation) return;
      adcCaptureId = started.capture_id;
      pollAdcCapture(adcCaptureId, generation);
    } catch (err) {
      if (adcCapturePollGeneration !== generation) return;
      adcCaptureBusy = false;
      adcCaptureError = describeError(err, 'start ADC capture');
    }
  }

  function openCalibration(sensor, measurement) {
    const channel = sensorStatus?.channels?.find((candidate) => candidate.id === sensor);
    const value = channel?.calibration?.[measurement];
    if (!value) return;
    selectSensor(sensor);
    calibrationEditor = {
      sensor, measurement,
      gain: value.gain, offset: value.offset_input_v,
      defaultGain: value.default_gain, defaultOffset: value.default_offset_input_v,
      chartUnits: 'engineering',
    };
    calibrationReference = '';
    calibrationError = calibrationMessage = '';
  }

  function adcInputFromEngineering(displayed, calibration) {
    if (!Number.isFinite(displayed) || !Number.isFinite(calibration?.gain) ||
        calibration.gain <= 0 || !Number.isFinite(calibration?.offset_input_v)) {
      return Number.NaN;
    }
    return displayed / calibration.gain + calibration.offset_input_v;
  }

  function rawCalibrationPoints(editor, sensor, channel, sensorReadings) {
    if (!editor || editor.sensor !== sensor) return [];
    const saved = channel?.calibration?.[editor.measurement];
    return sensorReadings.map((point) => ({
      ...point,
      raw_input_v: adcInputFromEngineering(point[editor.measurement], saved),
    }));
  }

  function previewCalibrationPoints(editor, sensor, channel, sensorReadings) {
    if (!editor || editor.sensor !== sensor) return [];
    const saved = channel?.calibration?.[editor.measurement];
    if (!saved || !Number.isFinite(editor.gain) || editor.gain <= 0) return [];
    return sensorReadings.map((point) => {
      const input = adcInputFromEngineering(point[editor.measurement], saved);
      return { ...point, preview: (input - editor.offset) * editor.gain };
    });
  }

  function zeroCalibration() {
    if (Number.isFinite(calibrationInput)) {
      calibrationEditor = { ...calibrationEditor, offset: calibrationInput };
    }
  }

  function calculateCalibration() {
    const reference = Number(calibrationReference);
    const denominator = calibrationInput - calibrationEditor.offset;
    if (!Number.isFinite(reference) || reference <= 0 || !Number.isFinite(denominator) || Math.abs(denominator) < 0.005) {
      calibrationError = 'Enter a positive reference while the measured input is away from the configured zero.';
      return;
    }
    calibrationEditor = { ...calibrationEditor, gain: reference / denominator };
    calibrationError = '';
  }

  function resetCalibrationEditor() {
    calibrationEditor = {
      ...calibrationEditor,
      gain: calibrationEditor.defaultGain,
      offset: calibrationEditor.defaultOffset,
    };
  }

  async function submitCalibration() {
    if (!calibrationEditor || !Number.isFinite(calibrationEditor.gain) ||
        calibrationEditor.gain <= 0 || calibrationEditor.gain > 100) {
      calibrationError = 'Gain must be a positive number no greater than 100.';
      return;
    }
    calibrationBusy = true;
    calibrationError = calibrationMessage = '';
    try {
      sensorStatus = await saveSensorCalibration({
        sensor: calibrationEditor.sensor,
        measurement: calibrationEditor.measurement,
        gain: Number(calibrationEditor.gain),
        offset_input_v: Number(calibrationEditor.offset),
      });
      calibrationMessage = 'Calibration saved.';
      calibrationEditor = null;
      // Retained observations were calculated with the previous coefficients;
      // do not reinterpret that mixed window if calibration is reopened.
      points = [];
    } catch (err) {
      calibrationError = describeError(err, 'save calibration');
    } finally {
      calibrationBusy = false;
    }
  }

  function sourceStateLabel(source) {
    if (!source) return 'Unknown';
    if (!source.transport) return 'Active';
    switch (source.transport.state) {
      case 'receiving': return 'Receiving';
      case 'initializing': return 'Initializing';
      case 'degraded': return 'Degraded';
      case 'unavailable': return 'Unavailable';
      case 'stale': return 'Stale';
      case 'waiting': return 'Waiting for data';
      default: return 'Unknown';
    }
  }

  function wifiStationLabel(station) {
    if (!station) return 'Loading…';
    if (station.state === 'connected_internet') return 'Connected · Internet available';
    if (station.state === 'connected_local') return 'Connected · Local network only';
    if (station.phase === 'obtaining_ip') return 'Connected · Obtaining IP address…';
    if (station.state === 'connecting' || station.phase === 'looking_for_network') return 'Connecting…';
    if (station.phase === 'retry_waiting') {
      return `Retrying in ${station.reconnect_seconds || 0}s · ${titleCase(station.failure)}`;
    }
    if (station.phase === 'action_required') return titleCase(station.failure);
    if (station.recovery === 'discovering') return 'Looking for saved networks…';
    if (station.recovery === 'blocked') return 'Saved networks need attention';
    return 'Disconnected';
  }

  function signalLabel(rssi) {
    if (!Number.isFinite(rssi)) return '';
    if (rssi >= -55) return `${rssi} dBm · Excellent`;
    if (rssi >= -67) return `${rssi} dBm · Good`;
    if (rssi >= -75) return `${rssi} dBm · Fair`;
    return `${rssi} dBm · Weak`;
  }

  // Turns a thrown error into a stable, user-facing message so different
  // failures (network down, server error, bad response) are distinguishable
  // instead of all collapsing into "undefined" or a raw stack trace.
  function describeError(err, action) {
    if (!err) return `Unable to ${action}.`;
    if (err.name === 'TypeError' && /fetch/i.test(err.message || '')) {
      return `Unable to ${action}: can't reach the meter on the network.`;
    }
    if (err.userMessage) return `Unable to ${action}: ${err.userMessage}`;
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
    clearTimeout(cycleRefreshTimer);
    clearInterval(wifiPollTimer);
    clearInterval(updatePollTimer);
    if (next !== 'raw') {
      clearTimeout(adcCapturePollTimer);
      adcCapturePollGeneration += 1;
      adcCaptureBusy = false;
    }

    if (next === 'history') {
      if (!history) {
        refreshHistory();
      } else {
        // Coming back to a route we already have data for — check whether
        // that data is stale enough to warrant an immediate refresh.
        scheduleHistoryRefresh();
      }
    }

    if (next === 'cycle') {
      if (!cycles) refreshCycles();
      else scheduleCycleRefresh();
    }

    if (next === 'remote' && hasTouchDisplay) {
      refreshRemote();
    }

    if (next === 'setup') refreshSetup();

    if (next === 'wifi') {
      refreshWifi();
      wifiPollTimer = setInterval(refreshWifi, WIFI_POLL_MS);
    }

    clearInterval(debugPollTimer);
    if (next === 'debug') {
      refreshDebug();
      debugPollTimer = setInterval(refreshDebug, DEBUG_POLL_MS);
    }

    if (next === 'info') {
      refreshUpdates();
      updatePollTimer = setInterval(refreshUpdates, UPDATE_POLL_MS);
    }

    if (next === 'sensors' || next === 'setup') {
      refreshSensors();
      scheduleSensorRefresh();
    } else {
      clearInterval(sensorPollTimer);
    }
    if (next === 'raw') resumeAdcCapture();
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
      statusFetchedAt = Date.now();
      applyWebTheme();
      if (route === 'setup' && !setup && setupMessage) refreshSetup();
      if (Number.isFinite(status?.ws_connection_limit)) {
        connectionLimit = status.ws_connection_limit;
      }
      syncHistoryRanges(status.time_source !== 'unanchored');
      statusError = '';
    } catch (err) {
      // A tab returning from the background can briefly outrun the browser's
      // restored network connection. The live-socket state already represents
      // that recovery, so only surface an independent REST failure while the
      // socket itself is healthy.
      statusError = connection === 'live' ? describeError(err, 'reach the meter') : '';
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

  function loadSetupDraft(value) {
    setup = value;
    setupBusy = false;
    setupHostname = value.hostname;
    setupSensorMode = value.sensor_mode;
    setupAppearance = value.appearance;
    setupStatusDisplayMode = value.status_display_mode || 'summary';
    resetSetup = resetWifi = resetCalibration = resetUsage = false;
    setupError = '';
    setupMessage = '';
  }

  async function refreshSetup() {
    try {
      loadSetupDraft(await getSetup());
    } catch (err) {
      setupError = describeError(err, 'load setup');
    }
  }

  function loadApDraft(value) {
    apEnabled = value.enabled;
    apSsid = value.ssid || '';
    apSecure = value.secure;
    apPassword = value.password || '';
    apDraftLoaded = true;
  }

  function apDraftMatches(value) {
    return !!value && apEnabled === value.enabled && apSsid === (value.ssid || '') &&
      apSecure === value.secure && apPassword === (value.password || '');
  }

  async function refreshWifi(syncAp = false) {
    try {
      const next = await getWifi();
      // Follow changes made on the touchscreen while the form is clean, but
      // never overwrite a browser edit that has not been applied yet.
      const shouldSyncAp = syncAp || !apDraftLoaded || apDraftMatches(wifi?.ap);
      wifi = next;
      if (shouldSyncAp) loadApDraft(next.ap);
      wifiError = '';
    } catch (err) {
      wifiError = describeError(err, 'load Wi-Fi settings');
    }
  }

  function selectStationNetwork(network) {
    stationSsid = network.ssid;
    stationSecure = network.secure;
    stationPassword = '';
  }

  async function stationCommand(command, successMessage) {
    wifiBusy = true;
    wifiError = '';
    wifiMessage = '';
    try {
      await sendWifiStationCommand(command);
      wifiMessage = successMessage;
      await refreshWifi();
    } catch (err) {
      wifiError = describeError(err, 'change station Wi-Fi');
    } finally {
      wifiBusy = false;
    }
  }

  function connectStation() {
    if (!stationSsid.length) {
      wifiError = 'Enter or select a network name.';
      return;
    }
    if ((stationSecure && stationPassword.length < 8) ||
        (stationPassword.length > 0 && stationPassword.length < 8)) {
      wifiError = 'Use an empty password for an open network, or at least 8 characters.';
      return;
    }
    stationCommand(
      { action: 'connect', ssid: stationSsid, password: stationPassword },
      `Connecting to ${stationSsid}…`,
    );
  }

  function connectSavedStation(ssid) {
    stationCommand({ action: 'connect_saved', ssid }, `Connecting to ${ssid}…`);
  }

  function forgetSavedStation(ssid) {
    if (!window.confirm(`Forget ${ssid}?`)) return;
    stationCommand({ action: 'forget', ssid }, `${ssid} forgotten.`);
  }

  async function submitAp() {
    wifiError = '';
    wifiMessage = '';
    if (apEnabled && !apSsid.length) {
      wifiError = 'Enter an access-point network name.';
      return;
    }
    if (apEnabled && apSecure && apPassword.length < 8) {
      wifiError = 'A secured access point needs a password of at least 8 characters.';
      return;
    }
    if (wifi?.ap?.enabled &&
        (!apEnabled || apSsid !== wifi.ap.ssid || apSecure !== wifi.ap.secure ||
         (apSecure && apPassword !== wifi.ap.password)) &&
        !window.confirm('Changing the active access point may disconnect this browser. Apply these settings?')) return;

    wifiBusy = true;
    try {
      await saveWifiAp({ enabled: apEnabled, ssid: apSsid, secure: apSecure, password: apPassword });
      wifiMessage = apEnabled
        ? 'Access-point settings applied. Reconnect to the new network if this browser disconnects.'
        : 'Access point stopped.';
      apDraftLoaded = false;
      await refreshWifi(true);
    } catch (err) {
      wifiError = describeError(err, 'change the access point');
    } finally {
      wifiBusy = false;
    }
  }

  function validHostname() {
    return /^[a-z0-9](?:[a-z0-9-]{0,29}[a-z0-9])?$/.test(setupHostname);
  }

  async function submitSetup() {
    setupError = '';
    setupMessage = '';
    if (!validHostname()) {
      setupError = 'Use lowercase letters, numbers, and hyphens (max 31); it cannot start or end with a hyphen.';
      return;
    }
    setupBusy = true;
    try {
      await saveSetup({
        hostname: setupHostname, sensor_mode: setupSensorMode, appearance: setupAppearance,
        status_display_mode: setupStatusDisplayMode,
        reset_setup: resetSetup, reset_wifi: resetWifi,
        reset_calibration: resetCalibration, reset_usage: resetUsage,
      });
      setupMessage = 'Settings saved. The device is restarting…';
      setup = null;
    } catch (err) {
      setupError = describeError(err, 'save setup');
      setupBusy = false;
    }
  }

  async function refreshDebug() {
    try {
      debugStatus = await getDebug();
      debugError = '';
    } catch (err) {
      debugError = describeError(err, 'load debug details');
    }
  }

  async function refreshUpdates() {
    try {
      updateStatus = await getUpdates();
      updateError = '';
    } catch (err) {
      updateError = describeError(err, 'load update status');
    }
  }

  async function checkUpdatesNow() {
    updateActionBusy = true;
    updateError = '';
    try {
      await checkForUpdates();
      await refreshUpdates();
    } catch (err) {
      updateError = describeError(err, 'check for updates');
    } finally {
      updateActionBusy = false;
    }
  }

  async function installAvailableUpdate() {
    updateActionBusy = true;
    updateError = '';
    try {
      await installUpdate();
      await refreshUpdates();
    } catch (err) {
      updateError = describeError(err, 'install the update');
      updateActionBusy = false;
    }
  }

  async function changeAutomaticUpdates(event) {
    const automatic = event.currentTarget.checked;
    updateActionBusy = true;
    updateError = '';
    try {
      await saveUpdateSettings(automatic);
      await refreshUpdates();
    } catch (err) {
      updateError = describeError(err, 'save automatic update settings');
    } finally {
      updateActionBusy = false;
    }
  }

  function updateStateLabel(value) {
    const labels = {
      idle: 'Ready', waiting_for_network: 'Waiting for Internet',
      waiting_for_time: 'Waiting for network time', checking: 'Checking…',
      up_to_date: 'Up to date', available: 'Update available',
      downloading: 'Downloading…', verifying: 'Verifying…',
      rebooting: 'Restarting…', failed: 'Check failed',
      blocked_after_rollback: 'Update blocked after rollback',
    };
    return labels[value] || 'Ready';
  }

  function formatCheckedAt(value) {
    return Number.isFinite(value) && value > 0
      ? new Date(value).toLocaleString()
      : 'Never';
  }

  function formatUpdateDate(value) {
    return Number.isFinite(value) && value > 0
      ? new Date(value).toLocaleDateString(undefined, {
          year: 'numeric', month: 'short', day: 'numeric',
        })
      : '—';
  }

  function debugHistoryLabel(query) {
    if (!query?.last_duration_ms) return 'No query yet';
    return `${query.was_usage ? 'Usage' : 'Files'} ${query.last_duration_ms}ms; ${query.files_read} files, ${query.records_read} rows (max ${query.max_duration_ms}ms)`;
  }

  function debugResetLabel(value) {
    const labels = {
      power_on: 'Power-on', software: 'Software', panic: 'Panic',
      interrupt_watchdog: 'Interrupt WDT', task_watchdog: 'Task WDT',
      watchdog: 'Other WDT', brownout: 'Brownout', other: 'Other',
    };
    return labels[value] || '—';
  }

  function otaHealthLabel(ota) {
    if (!ota) return '—';
    if (ota.validation_remaining_ms > 0) return `${ota.health}; verify ${(ota.validation_remaining_ms / 1000).toFixed(1)} s`;
    return ota.health === 'confirmed' ? 'confirmed; verified' : ota.health;
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

    const timestamp = Number.isFinite(frame.unixMs) ? frame.unixMs : frame.uptimeMs;
    const receivedAt = Date.now();
    const receivedMonotonicAt = performance.now();
    const eligible = (index) => !!(frame.eligibleMask & (1 << index));
    const observed = (index) => !!(frame.observedMask & (1 << index));
    const liveSensor = (index, reading) => {
      if (!observed(index)) return {};
      return {
        ...reading,
        duty: Number.isFinite(reading.duty) ? reading.duty * 100 : Number.NaN,
      };
    };
    points = [
      ...points,
      {
        in: eligible(0) ? frame.in.power : Number.NaN,
        out: eligible(1) ? frame.out.power : Number.NaN,
        timestamp, receivedAt, receivedMonotonicAt,
        sensors: {
          in: liveSensor(0, frame.in),
          out: liveSensor(1, frame.out),
          aux: liveSensor(2, frame.aux),
        },
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

    if (limit) connectionLimit = limit;

    if (state === 'live') {
      connection = 'live';
      reconnectAttempts = 0;
      statusError = '';
      return;
    }

    if (state === 'limited') {
      connection = 'limited';
      reconnectAttempts = 0;
      statusError = '';
      return;
    }

    // WebSocket errors are followed by a close event. Move the indicator to
    // its transient state immediately, but let close own retry scheduling so
    // one failed connection produces only one backoff step.
    if (state === 'reconnecting') {
      if (reconnectAttempts < RECONNECT_FAIL_AFTER_ATTEMPTS) {
        connection = 'reconnecting';
      }
      statusError = '';
      return;
    }

    if (state === 'offline') {
      // If the tab is hidden, don't burn reconnect attempts in the
      // background — we resume cleanly when it becomes visible again.
      if (document.hidden) return;

      reconnectAttempts += 1;
      connection = reconnectAttempts >= RECONNECT_FAIL_AFTER_ATTEMPTS
        ? 'failed'
        : 'reconnecting';
      statusError = '';

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
    connection = 'reconnecting';
    statusError = '';
    connectLive();
  }

  // ---------------------------------------------------------------------
  // Energy cycles
  // ---------------------------------------------------------------------

  async function refreshCycles({ silent = false } = {}) {
    clearTimeout(cycleRefreshTimer);
    if (!silent) cycleBusy = true;
    try {
      cycles = await getCycles();
      cycleEndHour = cycles.end_hour;
      cycleFetchedAt = Date.now();
      cycleError = '';
      cycleBusy = false;
      scheduleCycleRefresh();
    } catch (err) {
      cycleError = describeError(err, 'load energy cycles');
      cycleBusy = false;
      if (!destroyed && route === 'cycle' && !document.hidden) {
        cycleRefreshTimer = setTimeout(() => refreshCycles({ silent: true }), 30_000);
      }
    }
  }

  function scheduleCycleRefresh() {
    clearTimeout(cycleRefreshTimer);
    if (destroyed || route !== 'cycle' || document.hidden) return;
    const elapsed = Date.now() - cycleFetchedAt;
    cycleRefreshTimer = setTimeout(() => refreshCycles({ silent: true }),
      Math.max(0, CYCLE_REFRESH_MS - elapsed));
  }

  async function selectCycleEnd(event) {
    const nextHour = Number(event.currentTarget.value);
    if (!Number.isInteger(nextHour) || nextHour < 0 || nextHour > 23) return;
    cycleBusy = true;
    try {
      await saveCycleEndHour(nextHour);
      cycleEndHour = nextHour;
      await refreshCycles({ silent: true });
    } catch (err) {
      cycleError = describeError(err, 'save cycle end time');
      cycleBusy = false;
    }
  }

  // ---------------------------------------------------------------------
  // History
  // ---------------------------------------------------------------------

  function syncHistoryRanges(hasTime) {
    if (hasTime === historyRangesHaveTime) return;
    historyRangesHaveTime = hasTime;
    historyRanges = hasTime ? clockHistoryRanges : relativeHistoryRanges;
    let nextRange = historyRanges.find((range) => range.id === historyRange.id);
    if (!nextRange) {
      nextRange = hasTime
        ? historyRanges.find((range) => range.id === 'today')
        : historyRanges.find((range) => range.id === 'sinceboot');
    }
    historyRange = nextRange || historyRanges[0];
    history = null;
    if (route === 'history' && !document.hidden) refreshHistory(historyRange);
  }

  async function refreshHistory(range = historyRange, { silent = false } = {}) {
    const generation = ++historyRequestGeneration;
    historyRange = range;
    clearTimeout(historyRefreshTimer);
    if (!silent) historyBusy = true;

    try {
      const result = await getHistory(range.id, range.minutes);
      if (generation !== historyRequestGeneration) return;
      history = result;
      historyFetchedAt = Date.now();
      historyError = '';
      historyBusy = false;
      scheduleHistoryRefresh();
    } catch (err) {
      if (generation !== historyRequestGeneration) return;
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

    // "All History" and "Since Boot" use firmware-selected bucket sizes;
    // other ranges have fixed buckets. Align to the device's monotonic minute
    // boundaries, just after storage closes the minute.
    const interval = historyRange.refreshMs ||
      (['all', 'sinceboot'].includes(historyRange.id) && history?.bucketMinutes
        ? history.bucketMinutes * 60_000
        : null);
    if (!interval) return; // this range is manual-refresh only

    const elapsed = Date.now() - historyFetchedAt;
    let delay = Math.max(0, interval - elapsed);
    if (Number.isFinite(status?.uptime_ms) && elapsed < interval) {
      const estimatedUptime = status.uptime_ms + Math.max(0, Date.now() - statusFetchedAt);
      const untilBoundary = interval - (estimatedUptime % interval);
      // Storage normally completes on the sample bracketing the boundary;
      // a short grace also covers its bounded late-sample path.
      delay = Math.min(delay, untilBoundary + 2_000);
    }
    historyRefreshTimer = setTimeout(() => refreshHistory(historyRange, { silent: true }), delay);
  }

  function selectHistory(event) {
    const range = historyRanges.find((item) => item.id === event.currentTarget.value);
    if (range) refreshHistory(range);
  }

  async function contributeBrowserTime() {
    if (await anchorTime()) await refreshStatus();
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
      clearTimeout(cycleRefreshTimer);
      clearTimeout(adcCapturePollTimer);
      adcCapturePollGeneration += 1;
      if (route === 'raw') adcCaptureBusy = false;
      clearInterval(sensorPollTimer);
      clearInterval(debugPollTimer);
      clearInterval(wifiPollTimer);
      clearInterval(updatePollTimer);
      return;
    }

    // Tab is visible again: resync the clock, reconnect live data, and
    // refresh whatever view is open if its data has gone stale.
    contributeBrowserTime();
    resumeLive();
    refreshStatus();

    if (route === 'history') scheduleHistoryRefresh();
    if (route === 'cycle') scheduleCycleRefresh();
    if (route === 'remote') refreshRemote();
    if (route === 'sensors' || route === 'setup') {
      refreshSensors();
      scheduleSensorRefresh();
    }
    if (route === 'raw') resumeAdcCapture();
    if (route === 'debug') {
      refreshDebug();
      debugPollTimer = setInterval(refreshDebug, DEBUG_POLL_MS);
    }
    if (route === 'wifi') {
      refreshWifi();
      wifiPollTimer = setInterval(refreshWifi, WIFI_POLL_MS);
    }
    if (route === 'info') {
      refreshUpdates();
      updatePollTimer = setInterval(refreshUpdates, UPDATE_POLL_MS);
    }
  }

  onMount(() => {
    destroyed = false;
    colorSchemeMedia = matchMedia('(prefers-color-scheme: dark)');
    colorSchemeMedia.addEventListener?.('change', colorSchemeChanged);
    applyWebTheme();
    refreshStatus();
    contributeBrowserTime();
    connectLive();
    enterRoute(route);

    statusPollTimer = setInterval(refreshStatus, STATUS_POLL_MS);

    document.addEventListener('visibilitychange', handleVisibilityChange);
    window.addEventListener('popstate', handlePopState);

    return () => {
      destroyed = true;
      clearInterval(statusPollTimer);
      clearInterval(sensorPollTimer);
      clearInterval(debugPollTimer);
      clearInterval(wifiPollTimer);
      clearInterval(updatePollTimer);
      clearTimeout(reconnectTimer);
      clearTimeout(remoteRefreshTimer);
      clearTimeout(historyRefreshTimer);
      clearTimeout(cycleRefreshTimer);
      clearTimeout(adcCapturePollTimer);
      adcCapturePollGeneration += 1;
      socket?.close();
      if (remoteImage) URL.revokeObjectURL(remoteImage);
      colorSchemeMedia?.removeEventListener?.('change', colorSchemeChanged);
      document.removeEventListener('visibilitychange', handleVisibilityChange);
      window.removeEventListener('popstate', handlePopState);
    };
  });
</script>

<main>
  <header class="appbar">
    <h1>{status?.hostname || 'meter'}</h1>
    <span
      class="connection"
      class:live={connection === 'live'}
      class:reconnecting={connection === 'connecting' || connection === 'reconnecting'}
      class:failed={connection === 'failed' || connection === 'limited'}
      class:paused={connection === 'paused'}
      aria-label={connectionLabel()}
      title={connectionLabel()}
    >
      <b></b>
    </span>
    {#if connection === 'failed'}
      <span class="connection-message" role="alert">Failed to connect</span>
    {:else if connection === 'limited'}
      <span class="connection-message">Connection limit reached ({connectionLimit})</span>
    {/if}
    <div class="theme-segments" aria-label="Web appearance">
      {#each webThemeOptions as option}
        <button class:active={webTheme === option} aria-pressed={webTheme === option} on:click={() => selectWebTheme(option)}>
          {option === 'device' ? 'Device' : titleCase(option)}
        </button>
      {/each}
    </div>
  </header>

  <nav class="main-nav" aria-label="Main navigation">
    <button class:active={route === 'sensors' || route === 'raw'}
      on:click={() => navigate('sensors')}>Sensors</button>
    <button class:active={route === 'overview'} on:click={() => navigate('overview')}>Power</button>
    <button class:active={route === 'history'} on:click={() => navigate('history')}>Usage</button>
    <button class:active={route === 'cycle'} on:click={() => navigate('cycle')}>Cycle</button>
    <button class:active={SETTINGS_ROUTES.includes(route)} on:click={() => navigate('setup')}>Settings</button>
  </nav>

  {#if SETTINGS_ROUTES.includes(route)}
    <nav class="settings-nav" aria-label="Settings navigation">
      <button class:active={route === 'wifi'} on:click={() => navigate('wifi')}>Wi-Fi</button>
      <button class:active={route === 'setup'} on:click={() => navigate('setup')}>Setup</button>
      <button class:active={route === 'info'} on:click={() => navigate('info')}>Info</button>
      <button class:active={route === 'debug'} on:click={() => navigate('debug')}>Debug</button>
      {#if hasTouchDisplay}
        <button class:active={route === 'remote'} on:click={() => navigate('remote')}>Remote</button>
      {/if}
    </nav>
  {/if}

  {#if statusError}<p class="error" role="alert">{statusError}</p>{/if}

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
  {:else if route === 'wifi'}
    <section class="wifi-view">
      {#if wifiError}<p class="error" role="alert">{wifiError}</p>{/if}
      {#if wifiMessage}<p class="success" role="status">{wifiMessage}</p>{/if}

      <div class="wifi-grid">
        <article class="wifi-card">
          <header class="wifi-heading">
            <div>
              <h2>Station</h2>
              <p class:good={wifi?.station?.state?.startsWith('connected')} class:warning={wifi?.station?.state === 'connecting'}>
                {wifiStationLabel(wifi?.station)}
              </p>
            </div>
            {#if wifi?.station?.state !== 'disconnected' || wifi?.station?.ssid}
              <button class="secondary compact" disabled={wifiBusy} on:click={() => stationCommand({ action: 'disconnect' }, 'Station disconnected.')}>Disconnect</button>
            {/if}
          </header>

          {#if wifi?.station?.ssid}
            <dl class="wifi-current">
              <dt>Network</dt><dd>{wifi.station.ssid}</dd>
              <dt>IP address</dt><dd>{wifi.station.ip || '—'}</dd>
              <dt>Signal</dt><dd>{signalLabel(wifi.station.rssi) || '—'}</dd>
            </dl>
          {/if}

          <div class="wifi-section-heading">
            <h3>Available networks</h3>
            <button class="secondary compact" disabled={wifiBusy || wifiScanBusy} on:click={() => stationCommand({ action: 'scan' }, 'Scanning for networks…')}>
              {wifiScanBusy ? 'Scanning…' : 'Scan'}
            </button>
          </div>
          {#if wifiNetworks.length}
            <div class="network-list">
              {#each wifiNetworks as network}
                <button type="button" class:selected={stationSsid === network.ssid} on:click={() => selectStationNetwork(network)}>
                  <span>{network.ssid || '(hidden network)'}</span>
                  <small>{network.secure ? 'Secured' : 'Open'} · {network.rssi} dBm</small>
                </button>
              {/each}
            </div>
          {:else}
            <p class="wifi-empty">{wifiScanBusy ? 'Scanning…' : 'Scan to find nearby networks.'}</p>
          {/if}

          <form class="wifi-connect" on:submit|preventDefault={connectStation}>
            <label for="station-ssid">Network name (SSID)</label>
            <input id="station-ssid" bind:value={stationSsid} on:input={() => stationSecure = false} maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false" />
            <label for="station-password">Password</label>
            <input id="station-password" type="password" bind:value={stationPassword} maxlength="63" autocomplete="current-password" placeholder={stationSecure ? 'Required for this network' : 'Leave empty for an open network'} />
            <button class="primary" type="submit" disabled={wifiBusy || wifiScanBusy || !stationSsid}>Connect</button>
          </form>

          <h3 class="saved-title">Saved networks</h3>
          {#if wifi?.saved_networks?.length}
            <div class="saved-networks">
              {#each wifi.saved_networks as ssid}
                <div>
                  <span>{ssid}</span>
                  <button class="secondary compact" disabled={wifiBusy} on:click={() => connectSavedStation(ssid)}>Connect</button>
                  <button class="danger compact" disabled={wifiBusy} on:click={() => forgetSavedStation(ssid)}>Forget</button>
                </div>
              {/each}
            </div>
          {:else}
            <p class="wifi-empty">No saved networks.</p>
          {/if}
        </article>

        <article class="wifi-card">
          <header class="wifi-heading">
            <div>
              <h2>Access point</h2>
              <p class:good={wifi?.ap?.enabled}>{wifi?.ap?.enabled ? `Running · ${wifi.ap.client_count} connected` : 'Stopped'}</p>
            </div>
          </header>
          {#if wifi?.ap?.enabled}
            <dl class="wifi-current">
              <dt>Network</dt><dd>{wifi.ap.ssid}</dd>
              <dt>IP address</dt><dd>{wifi.ap.ip || '—'}</dd>
            </dl>
          {/if}

          <form class="wifi-ap-form" on:submit|preventDefault={submitAp}>
            <fieldset disabled={wifiBusy}>
              <label class="switch-row"><input type="checkbox" bind:checked={apEnabled} /> Enable access point</label>
              <label for="ap-ssid">Network name (SSID)</label>
              <input id="ap-ssid" bind:value={apSsid} maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false" />
              <label class="switch-row"><input type="checkbox" bind:checked={apSecure} /> Require a password</label>
              <label for="ap-password">Access-point password</label>
              <input id="ap-password" type="password" bind:value={apPassword} maxlength="63" minlength={apSecure ? 8 : undefined} disabled={!apSecure} autocomplete="new-password" />
            </fieldset>
            <button class="primary" type="submit" disabled={wifiBusy}>{wifiBusy ? 'Applying…' : 'Apply access-point settings'}</button>
          </form>

          <h3 class="saved-title">Connected devices</h3>
          {#if wifi?.ap?.clients?.length}
            <ul class="ap-clients">
              {#each wifi.ap.clients as mac}<li>{mac}</li>{/each}
            </ul>
          {:else}
            <p class="wifi-empty">No devices connected.</p>
          {/if}
          <p class="field-note">Changing or stopping the access point disconnects browsers using that network.</p>
        </article>
      </div>
    </section>
  {:else if route === 'setup'}
    <section class="setup-view">
      {#if setupError}<p class="error" role="alert">{setupError}</p>{/if}
      {#if setupMessage}<p class="success" role="status">{setupMessage}</p>{/if}
      {#if sensorStatusError}<p class="error" role="alert">{sensorStatusError}</p>{/if}
      {#if setup}
        <form on:submit|preventDefault={submitSetup}>
          <fieldset disabled={setupBusy}>
            <legend>Sensor mode</legend>
            <div class="form-segments">
              {#each sensorModeOptions as option}
                <button type="button" class:active={setupSensorMode === option[0]} aria-pressed={setupSensorMode === option[0]} on:click={() => setupSensorMode = option[0]}>{option[1]}</button>
              {/each}
            </div>
            <p class="field-note">Active: {sensorStatus?.source?.label || '—'} · {sourceStateLabel(sensorStatus?.source)}</p>

            {#if hasTouchDisplay}
              <div class="field-label">Device appearance</div>
              <div class="form-segments">
                {#each ['light', 'dark', 'auto'] as option}
                  <button type="button" class:active={setupAppearance === option} aria-pressed={setupAppearance === option} on:click={() => setupAppearance = option}>{titleCase(option)}</button>
                {/each}
              </div>
            {/if}

            {#if hasStatusDisplay}
              <div class="field-label">Status display</div>
              <div class="form-segments">
                {#each ['summary', 'dense'] as option}
                  <button type="button" class:active={setupStatusDisplayMode === option} aria-pressed={setupStatusDisplayMode === option} on:click={() => setupStatusDisplayMode = option}>{titleCase(option)}</button>
                {/each}
              </div>
              <p class="field-note">Summary uses larger text; Dense shows full network and sensor diagnostics.</p>
            {/if}

            <label class="field-label" for="hostname">Hostname</label>
            <input id="hostname" bind:value={setupHostname} maxlength="31" pattern="[a-z0-9](?:[a-z0-9-]{0,29}[a-z0-9])?" autocapitalize="none" autocomplete="off" spellcheck="false" />

            <div class="field-label">Reset</div>
            <div class="reset-options">
              <label><input type="checkbox" bind:checked={resetSetup} /> Setup</label>
              <label><input type="checkbox" bind:checked={resetWifi} /> Wi-Fi</label>
              <label><input type="checkbox" bind:checked={resetCalibration} /> Sensor Calibration</label>
              <label><input type="checkbox" bind:checked={resetUsage} /> Usage Data</label>
            </div>
          </fieldset>

          <p class="save-warning">Saving applies all selected changes and restarts the device.</p>
          <div class="form-actions">
            <button type="button" class="secondary" disabled={!setupIsDirty || setupBusy} on:click={() => loadSetupDraft(setup)}>Discard</button>
            <button type="submit" class="primary" disabled={!setupIsDirty || setupBusy}>{setupBusy ? 'Applying…' : 'Save'}</button>
          </div>
        </form>
      {:else if !setupMessage}
        <p class="empty">Loading setup…</p>
      {/if}
    </section>
  {:else if route === 'info'}
    <section class="table-view info-view">
      <div>
        <h2>Device info</h2>
        <dl class="striped-details">
          <dt>Uptime</dt><dd>{formatUptime(status?.uptime_ms)}</dd>
          <dt>Date</dt><dd>{status?.date || '—'}</dd>
          <dt>Time</dt><dd>{status?.time || '—'}</dd>
          <dt>IP Address ({status?.network?.station_ssid || 'Station'})</dt><dd>{status?.network?.station_ip || '—'}</dd>
          <dt>IP Address ({status?.network?.ap_ssid || 'Access Point'})</dt><dd>{status?.network?.ap_ip || '—'}</dd>
        </dl>
      </div>
      <article class="update-card">
        <div class="update-heading">
          <div>
            <h2>Software update</h2>
            <span class="state">{updateStateLabel(updateStatus?.state)}</span>
          </div>
          <label class="switch-row">
            <input type="checkbox" checked={updateStatus?.automatic !== false}
              disabled={updateActionBusy}
              on:change={changeAutomaticUpdates} />
            Install updates automatically
          </label>
        </div>
        {#if updateError}<p class="error" role="alert">{updateError}</p>{/if}
        {#if updateStatus?.error}<p class="error" role="alert">{updateStatus.error}</p>{/if}
        <dl class="update-details">
          <dt>Version</dt><dd>v{updateStatus?.current_version || status?.build_version || '—'}</dd>
          <dt>Build Date</dt><dd>{status?.build_date || '—'}</dd>
          <dt>Update Date</dt><dd>{formatUpdateDate(updateStatus?.update_date_unix_ms)}</dd>
          <dt>Available</dt><dd>{updateStatus?.available_version ? `v${updateStatus.available_version}` : '—'}</dd>
          <dt>Last checked</dt><dd>{formatCheckedAt(updateStatus?.last_check_unix_ms)}</dd>
        </dl>
        {#if updateStatus?.state === 'downloading'}
          <progress max="100" value={updateStatus.progress_percent || 0}>
            {updateStatus.progress_percent || 0}%
          </progress>
        {/if}
        {#if updateStatus?.state === 'blocked_after_rollback'}
          <p class="field-note">This version was rolled back and will not be retried automatically. Publish a newer release after correcting it.</p>
        {/if}
        <div class="update-actions">
          <button class="secondary" type="button"
            disabled={updateActionBusy || updateStatus?.busy}
            on:click={checkUpdatesNow}>Check now</button>
          {#if updateStatus?.state === 'available'}
            <button class="primary" type="button"
              disabled={updateActionBusy || updateStatus?.busy}
              on:click={installAvailableUpdate}>Install v{updateStatus.available_version}</button>
          {/if}
        </div>
        <p class="field-note">Installation pauses live streaming and restarts the meter after the signed image is verified.</p>
        <footer class="update-contact" aria-label="Project and contact information">
          <a href="https://github.com/camlee/power-meter" target="_blank" rel="noreferrer">GitHub repository</a>
          <span aria-hidden="true">·</span>
          <a href="mailto:cam.w.lee@gmail.com">cam.w.lee@gmail.com</a>
        </footer>
      </article>
    </section>
  {:else if route === 'debug'}
    <section class="table-view">
      <h2>Debug</h2>
      {#if debugError}<p class="error" role="alert">{debugError}</p>{/if}
      <dl class="striped-details">
        {#if hasTouchDisplay}<dt>LVGL</dt><dd>{debugStatus?.lvgl || '—'}</dd>{/if}
        <dt>ESP-IDF / SDK</dt><dd>{debugStatus?.sdk || '—'}</dd>
        <dt>Chip</dt><dd>{debugStatus?.chip || '—'}</dd>
        <dt>CPU / flash</dt><dd>{debugStatus ? `${debugStatus.cpu_mhz} MHz / ${debugStatus.flash_mb} MB flash` : '—'}</dd>
        <dt>Last reset</dt><dd>{debugResetLabel(debugStatus?.last_reset)}</dd>
        <dt>Time source</dt><dd>{debugStatus?.time_source || 'unanchored'}</dd>
        <dt>Web build</dt><dd>{debugStatus?.web_build || '—'}</dd>
        <dt>Internal heap</dt><dd>{debugStatus ? `${debugStatus.internal_heap.used_percent}% used; max ${debugStatus.internal_heap.largest_free_kb}K` : '—'}</dd>
        <dt>PSRAM heap</dt><dd>{debugStatus ? `${debugStatus.psram_heap.used_percent}% used; max ${debugStatus.psram_heap.largest_free_kb}K` : '—'}</dd>
        <dt>Data storage</dt><dd>{debugStatus ? (debugStatus.storage.mounted ? `${debugStatus.storage.used_kb} KB / ${debugStatus.storage.total_kb} KB` : 'Unmounted') : '—'}</dd>
        <dt>WS connections</dt><dd>{debugStatus?.ws_connections ?? '—'} / {debugStatus?.ws_connection_limit ?? '—'}</dd>
        <dt>OTA</dt><dd>{otaHealthLabel(debugStatus?.ota)}</dd>
        <dt>OTA slots</dt><dd>{debugStatus ? `${debugStatus.ota.running_slot} → ${debugStatus.ota.boot_slot}` : '—'}</dd>
        <dt>OTA image</dt><dd>{debugStatus ? `${debugStatus.ota.image_state}${debugStatus.ota.rollback_detected ? '; rollback detected' : ''}` : '—'}</dd>
        <dt>History query</dt><dd>{debugHistoryLabel(debugStatus?.history_query)}</dd>
      </dl>
    </section>
  {:else if route === 'raw'}
    <section class="adc-capture-view" aria-live="polite">
      <div class="adc-capture-toolbar">
        <button class="compact secondary back-button" type="button"
          on:click={() => navigate('sensors')} aria-label="Return to sensors">← Sensors</button>
        <div>
          <h2>{sensorLabel(adcCapture?.channel || adcCaptureSensor)} sensor raw capture</h2>
          <p>Calibrated voltage and current observations used by the production reducer.</p>
        </div>
        <button class="primary compact" type="button" disabled={adcCaptureBusy}
          on:click={() => startAdcCapture(adcCaptureSensor)}>
          {adcCaptureBusy ? (adcCaptureState === 'capturing' ? 'Capturing…' : 'Arming…') : 'Capture again'}
        </button>
      </div>

      {#if adcCaptureBusy}
        <div class="adc-capture-progress">
          <span class="progress" role="status" aria-label="Capturing ADC readings"></span>
          <p>Collecting three 500 ms windows…</p>
        </div>
      {/if}
      {#if adcCaptureError}<p class="error" role="alert">{adcCaptureError}</p>{/if}
      {#if adcCapture}
        <div class="adc-capture-heading">
          <p>Raw observations and their three 500 ms production values.</p>
          <span>{adcCapture.points.length} samples ·
            {adcCapture.measuredIntervalUs > 0
              ? `${(1_000_000 / adcCapture.measuredIntervalUs).toFixed(1)} Hz`
              : 'rate unavailable'}</span>
        </div>
        <AdcCaptureChart capture={adcCapture} />
        <div class="adc-window-table-wrap">
          <table class="adc-window-table">
            <thead><tr><th>Window</th><th>State</th><th>Voltage</th><th>Current</th><th>Power</th><th>Duty</th></tr></thead>
            <tbody>
              {#each adcCapture.windows as window, index}
                <tr>
                  <th>{index + 1}</th>
                  <td class:warning={window.state === 'out_of_range'}
                    class:bad={!window.eligible && window.state !== 'out_of_range'}>
                    {stateLabel(window.state)}
                  </td>
                  <td>{formatMeasurement(window.voltage, 'V')}</td>
                  <td>{formatMeasurement(window.current, 'A')}</td>
                  <td>{formatMeasurement(window.power, 'W')}</td>
                  <td>{formatPercent(window.duty)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <p class="field-note">Requested interval:
          {(adcCapture.requestedIntervalUs / 1000).toFixed(1)} ms · measured:
          {(adcCapture.measuredIntervalUs / 1000).toFixed(2)} ms · dropped:
          {adcCapture.droppedPoints}</p>
      {:else if !adcCaptureBusy && !adcCaptureError}
        <p class="empty">No capture is available.</p>
      {/if}
    </section>
  {:else if route === 'sensors'}
    <section class="sensors-view" aria-live="polite">
      {#if sensorStatusError}<p class="error" role="alert">{sensorStatusError}</p>{/if}
      <div class="sensor-heading">
        <div>
          <h2>Live sensors</h2>
          <p>{sensorStatus?.source?.label || '—'} source · {sourceStateLabel(sensorStatus?.source)}</p>
        </div>
      </div>
      <div class="sensor-tabs" role="tablist" aria-label="Sensor channel">
        {#each sensorStatus?.channels || [] as channel}
          <button type="button" role="tab" class:active={selectedSensor === channel.id}
            aria-selected={selectedSensor === channel.id} on:click={() => selectSensor(channel.id)}>
            {channel.label}
          </button>
        {/each}
      </div>
      {#if sensorStatus?.channels?.length}
        <div class="sensor-grid" class:calibrating={!!calibrationEditor}>
          {#each sensorStatus.channels as channel}
            {#if !calibrationEditor || calibrationEditor.sensor === channel.id}
              <section class="sensor-panel" class:active={selectedSensor === channel.id}>
                {#if calibrationEditor?.sensor !== channel.id}
                  <article class="sensor-card" class:warning={channel.state === 'out_of_range'}
                    class:bad={channel.state === 'invalid' || channel.state === 'stale'}>
                    <header>
                      <h3>{channel.label}</h3>
                      <div class="sensor-card-status">
                        <span class="state" class:good={channel.state === 'valid'}
                          class:warning={channel.state === 'out_of_range'}
                          class:bad={channel.state === 'invalid' || channel.state === 'stale'}>{stateLabel(channel.state)}</span>
                        {#if channel.configured &&
                          ['adc', 'ads1115'].includes(sensorStatus?.source?.mode)}
                          <button class="compact secondary" type="button"
                            on:click={() => startAdcCapture(channel.id)}>View Raw</button>
                        {/if}
                      </div>
                    </header>
                    <dl class="measurements kpis">
                      <dt>Voltage</dt><dd>{formatMeasurement(channel.voltage, 'V')}</dd>
                      <dt>Current</dt><dd>{formatMeasurement(channel.current, 'A')}</dd>
                      <dt>Power</dt><dd>{formatMeasurement(channel.power, 'W')}</dd>
                      <dt>Duty</dt><dd>{channel.duty?.state === 'valid' ? formatPercent(channel.duty.value) : '—'}</dd>
                    </dl>
                    {#if !channel.configured}<p class="sensor-note">This channel is not configured by the active source.</p>{/if}
                  </article>
                {/if}

                <div class="sensor-charts">
                  {#if calibrationEditor?.sensor === channel.id}
                    <div class="calibration-unit-toggle">
                      <span>Chart units</span>
                      <div role="group" aria-label="Calibration chart units">
                        <button type="button"
                          class:active={calibrationEditor.chartUnits === 'engineering'}
                          aria-pressed={calibrationEditor.chartUnits === 'engineering'}
                          on:click={() => calibrationEditor = {
                            ...calibrationEditor, chartUnits: 'engineering',
                          }}>Engineering ({calibrationEditor.measurement === 'voltage' ? 'V' : 'A'})</button>
                        <button type="button"
                          class:active={calibrationEditor.chartUnits === 'raw'}
                          aria-pressed={calibrationEditor.chartUnits === 'raw'}
                          on:click={() => calibrationEditor = {
                            ...calibrationEditor, chartUnits: 'raw',
                          }}>Raw ADC</button>
                      </div>
                    </div>
                    <SensorChart points={calibrationChartPoints}
                      field={calibrationEditor.chartUnits === 'raw'
                        ? 'raw_input_v' : calibrationEditor.measurement}
                      title={calibrationEditor.chartUnits === 'raw'
                        ? `${calibrationEditor.measurement === 'voltage' ? 'Voltage' : 'Current'} ADC input`
                        : (calibrationEditor.measurement === 'voltage' ? 'Voltage' : 'Current')}
                      unit={calibrationEditor.chartUnits === 'raw'
                        ? 'V' : (calibrationEditor.measurement === 'voltage' ? 'V' : 'A')}
                      colorVariable={calibrationEditor.measurement === 'voltage' ? '--panel' : '--warning'}
                      active={!livePaused && selectedSensor === channel.id}
                      yMin={calibrationEditor.chartUnits === 'raw' ? 0 : null}
                      yMax={calibrationEditor.chartUnits === 'raw' ? 3.3 : null}
                      previewPoints={calibrationEditor.chartUnits === 'raw'
                        ? [] : calibrationPreviewPoints}
                      showPreviewLegend={calibrationEditor.chartUnits !== 'raw'} />
                  {:else}
                    <SensorChart points={sensorPoints[channel.id] || []} field="voltage" title="Voltage" unit="V"
                      colorVariable="--panel" active={!livePaused && selectedSensor === channel.id} />
                    <SensorChart points={sensorPoints[channel.id] || []} field="current" title="Current" unit="A"
                      colorVariable="--warning" active={!livePaused && selectedSensor === channel.id} />
                    <SensorChart points={sensorPoints[channel.id] || []} field="power" title="Power" unit="W"
                      colorVariable="--charge" active={!livePaused && selectedSensor === channel.id} />
                    <SensorChart points={sensorPoints[channel.id] || []} field="duty" title="Duty" unit="%"
                      colorVariable="--surplus" active={!livePaused && selectedSensor === channel.id}
                      emptyMessage={dutyEmptyMessage(channel)} />
                  {/if}
                </div>

                {#if channel.calibration?.editable && calibrationEditor?.sensor !== channel.id}
                  <div class="calibration-actions">
                    <button type="button" on:click={() => openCalibration(channel.id, 'voltage')}>Calibrate voltage</button>
                    <button type="button" on:click={() => openCalibration(channel.id, 'current')}>Calibrate current</button>
                  </div>
                {/if}
              </section>
            {/if}
          {/each}

          {#if calibrationEditor}
            <form class="calibration-editor" on:submit|preventDefault={submitCalibration}>
              <h3>Calibrate {selectedChannel.label} {calibrationEditor.measurement}</h3>
              <label>ADC offset / zero (V)
                <input type="number" step="0.0001" min="0" max="3.3" bind:value={calibrationEditor.offset} />
              </label>
              <label>Gain ({calibrationEditor.measurement === 'voltage' ? 'V' : 'A'} per ADC V)
                <input type="number" step="0.001" min="0.001" max="100" bind:value={calibrationEditor.gain} />
              </label>
              <div class="calibration-reference">
                <label>Trusted reference ({calibrationEditor.measurement === 'voltage' ? 'V' : 'A'})
                  <input type="number" step="0.001" min="0" bind:value={calibrationReference} />
                </label>
                <button type="button" on:click={calculateCalibration}>Calculate</button>
              </div>
              <p class="field-note">Raw ADC values are measured before offset and gain.
                Latest input: {formatMeasurement(calibrationInput, 'V ADC', 4)}</p>
              {#if calibrationError}<p class="error" role="alert">{calibrationError}</p>{/if}
              <div class="calibration-editor-actions">
                <button type="button" on:click={zeroCalibration}>Use latest as zero</button>
                <button type="button" on:click={resetCalibrationEditor}>Defaults</button>
                <button type="button" on:click={() => calibrationEditor = null}>Cancel</button>
                <button class="primary" type="submit" disabled={calibrationBusy}>{calibrationBusy ? 'Saving…' : 'Save'}</button>
              </div>
            </form>
          {/if}
        </div>

        {#if calibrationMessage}<p class="success" role="status">{calibrationMessage}</p>{/if}

      {/if}
      {#if !sensorStatus && !sensorStatusError}<p class="empty">Loading sensors…</p>{/if}
    </section>
  {:else if route === 'cycle'}
    <section class="cycle-view">
      <div class="cycle-heading">
        <h2>Energy Balance</h2>
        <label>
          <span class="sr-only">Cycle end time</span>
          <select value={cycleEndHour} on:change={selectCycleEnd} disabled={cycleBusy}>
            {#each cycleHours as option}
              <option value={option.hour}>{option.label}</option>
            {/each}
          </select>
        </label>
      </div>
      <p class="cycle-efficiency">Assuming 80% charge efficiency</p>

      {#if cycleError}<p class="error" role="alert">{cycleError}</p>{/if}
      {#if cycleBusy}<span class="progress" role="status" aria-label="Loading energy cycles"></span>{/if}

      <div class="cycle-summaries">
        <article>
          <span>7-DAY NET</span>
          <strong class={cycleNetClass(sevenDayNet)}>{formatCycleEnergy(sevenDayNet, true)}</strong>
        </article>
        <article>
          <span>TODAY SO FAR</span>
          <strong class={cycleNetClass(currentCycle?.net_wh)}>{formatCycleEnergy(currentCycle?.net_wh, true)}</strong>
        </article>
      </div>

      {#if displayedCycles.length}
        <div class="cycle-table-wrap">
          <table class="cycle-table">
            <thead><tr><th>Day</th><th>Charged</th><th>Used</th><th>Net</th></tr></thead>
            <tbody>
              {#each displayedCycles as cycle}
                <tr>
                  <th scope="row"><span>{cycleDay(cycle)}</span><span class="cycle-warning" aria-label={cycle.incomplete ? 'Incomplete data' : ''}>{cycle.incomplete ? '⚠' : ''}</span></th>
                  <td>{formatCycleEnergy(cycle.charged_wh)}</td>
                  <td>{formatCycleEnergy(cycle.used_wh)}</td>
                  <td class={cycleNetClass(cycle.net_wh)}>{formatCycleEnergy(cycle.net_wh, false, true)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        {#if displayedCycles.some((cycle) => cycle.incomplete)}
          <p class="cycle-incomplete">⚠ Incomplete Data</p>
        {/if}
      {:else if !cycleBusy}
        <p class="empty">No cycle data available.</p>
      {/if}
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
        <HistoryChart
          buckets={history.buckets}
          timelineBasis={history.timelineBasis}
          startTimeMs={history.startTimeMs}
          endTimeMs={history.endTimeMs}
          tickMinutes={historyRange.tickMinutes}
        />
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
    color-scheme: light;
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
    /* Override the inline boot stylesheet's prefers-color-scheme background
       once the explicit browser theme has resolved. */
    background-color: var(--background);
  }

  :global(:root[data-theme='dark']) {
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

  :global(html),
  :global(body),
  :global(#app) {
    width: 100%;
    height: 100%;
    background-color: var(--background);
  }

  :global(body) {
    margin: 0;
    overflow: hidden;
    color: var(--text);
    font: 16px/1.4 system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  }

  /* The inline boot screen centers #app with grid. Reset that shell after
     Svelte mounts so main owns the full viewport, including overflow. */
  :global(#app) {
    display: block;
  }

  main {
    width: 100%;
    max-width: 80rem;
    height: 100vh;
    height: 100dvh;
    min-height: 0;
    margin: auto;
    padding: 0.5rem 0.75rem;
    display: flex;
    flex-direction: column;
    overflow: hidden;
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

  .theme-segments {
    display: flex;
    margin-left: auto;
    border: 1px solid var(--border);
    border-radius: 0.35rem;
    overflow: hidden;
  }

  .theme-segments button {
    padding: 0.24rem 0.42rem;
    border: 0;
    border-left: 1px solid var(--border);
    font-size: 0.7rem;
  }

  .theme-segments button:first-child { border-left: 0; }
  .theme-segments button.active,
  .form-segments button.active {
    background: var(--accent);
    color: white;
  }

  .connection b {
    display: block;
    width: 0.58rem;
    height: 0.58rem;
    border-radius: 50%;
    background: var(--load);
  }

  .connection.reconnecting b {
    background: var(--warning);
    animation: reconnect-pulse 1s ease-in-out infinite;
  }

  .connection.live b {
    background: var(--charge);
  }

  .connection.failed b {
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

  .main-nav {
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
    cursor: default;
  }

  .main-nav button,
  .settings-nav button {
    padding: 0.55rem 0.68rem;
  }

  .main-nav button.active,
  .settings-nav button.active {
    color: var(--accent);
    box-shadow: inset 0 -2px var(--accent);
  }

  .settings-nav {
    display: flex;
    overflow-x: auto;
    gap: 0.25rem;
    border-bottom: 1px solid var(--border);
  }

  .settings-nav button {
    font-size: 0.8rem;
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
  .cycle-view,
  .remote-view,
  .wifi-view,
  .setup-view,
  .sensors-view,
  .adc-capture-view,
  .table-view {
    flex: 1;
    min-height: 0;
    margin-top: 0.55rem;
    overflow-y: auto;
  }

  .live-view,
  .history-view {
    display: flex;
    flex-direction: column;
    overflow: hidden;
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

  .cycle-view {
    width: 100%;
    max-width: 48rem;
    margin-left: auto;
    margin-right: auto;
  }

  .cycle-heading {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
  }

  .cycle-heading h2 { margin: 0; font-size: 1.25rem; }

  .cycle-heading select {
    min-width: 7.5rem;
    padding: 0.45rem 1.8rem 0.45rem 0.6rem;
    border: 1px solid var(--border);
    border-radius: 0.35rem;
    background: var(--surface);
    color: var(--text);
    font: 600 0.9rem inherit;
  }

  .cycle-efficiency {
    margin-top: 0.12rem;
    color: var(--muted);
    font-size: 0.8rem;
  }

  .cycle-summaries {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.65rem;
    margin: 1rem 0 0.8rem;
  }

  .cycle-summaries article {
    padding: 0.7rem;
    text-align: center;
    border: 1px solid var(--border);
    border-radius: 0.4rem;
    background: var(--surface);
  }

  .cycle-summaries span {
    display: block;
    color: var(--muted);
    font-size: 0.72rem;
    font-weight: 700;
    letter-spacing: 0.02em;
  }

  .cycle-summaries strong {
    display: block;
    margin-top: 0.15rem;
    font-size: clamp(1.35rem, 5vw, 2rem);
    font-weight: 500;
  }

  .cycle-table-wrap {
    overflow-x: auto;
    border: 1px solid var(--border);
    border-radius: 0.4rem;
  }

  .cycle-table {
    width: 100%;
    border-collapse: collapse;
    font-variant-numeric: tabular-nums;
  }

  .cycle-table th,
  .cycle-table td {
    padding: 0.72rem 0.85rem;
    text-align: right;
    white-space: nowrap;
  }

  .cycle-table thead th {
    color: var(--muted);
    font-size: 0.78rem;
    font-weight: 700;
  }

  .cycle-table th:first-child { text-align: left; }
  .cycle-table tbody th { font-size: 1.02rem; font-weight: 500; }
  .cycle-table tbody tr:nth-child(odd) { background: var(--surface); }

  .cycle-table tbody th {
    display: grid;
    grid-template-columns: 4.2rem 1rem;
    align-items: center;
  }

  .cycle-warning {
    color: var(--muted);
    font-size: 0.65rem;
    text-align: center;
  }

  .positive { color: var(--charge); }
  .negative { color: var(--battery); }
  .neutral { color: var(--text); }
  .unavailable { color: var(--muted); }

  .cycle-incomplete {
    margin-top: 0.65rem;
    color: var(--muted);
    font-size: 0.8rem;
    text-align: center;
  }

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
    overflow: hidden;
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

  .wifi-view { max-width: 68rem; }

  .wifi-grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 0.8rem;
    align-items: start;
  }

  .wifi-card {
    min-width: 0;
    padding: 0.85rem;
    border: 1px solid var(--border);
    border-radius: 0.4rem;
    background: var(--surface);
  }

  .wifi-heading,
  .wifi-section-heading {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.7rem;
  }

  .wifi-heading h2,
  .wifi-section-heading h3 { margin: 0; }

  .wifi-heading p {
    margin-top: 0.12rem;
    color: var(--muted);
    font-size: 0.8rem;
  }

  .wifi-heading p.good { color: var(--charge); }
  .wifi-heading p.warning { color: var(--warning); }

  .wifi-current {
    margin-top: 0.7rem;
    padding: 0.6rem;
    border: 1px solid var(--border);
    border-radius: 0.3rem;
    background: var(--background);
    font-size: 0.82rem;
  }

  .wifi-current dd { text-align: right; }

  .wifi-section-heading { margin-top: 1rem; }

  button.compact {
    padding: 0.32rem 0.55rem;
    border-radius: 0.28rem;
    font-size: 0.76rem;
  }

  button.secondary { border-color: var(--border); color: var(--text); }
  button.danger { border-color: var(--battery); color: var(--battery); }
  button.primary {
    min-height: 2.3rem;
    border-radius: 0.3rem;
    background: var(--accent);
    color: white;
  }

  .network-list {
    max-height: 12rem;
    margin-top: 0.45rem;
    overflow-y: auto;
    border: 1px solid var(--border);
    border-radius: 0.3rem;
    background: var(--background);
  }

  .network-list button {
    width: 100%;
    padding: 0.48rem 0.6rem;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.7rem;
    border-bottom: 1px solid var(--border);
    color: var(--text);
    text-align: left;
  }

  .network-list button:last-child { border-bottom: 0; }
  .network-list button.selected { background: color-mix(in srgb, var(--accent) 15%, var(--background)); }
  .network-list small { color: var(--muted); font-weight: 400; }

  .wifi-empty {
    margin-top: 0.45rem;
    color: var(--muted);
    font-size: 0.82rem;
  }

  .wifi-connect,
  .wifi-ap-form {
    display: grid;
    gap: 0.35rem;
    margin-top: 0.8rem;
  }

  .wifi-connect label,
  .wifi-ap-form label:not(.switch-row) {
    margin-top: 0.35rem;
    color: var(--muted);
    font-size: 0.76rem;
    font-weight: 700;
  }

  .wifi-connect input,
  .wifi-ap-form input[type='password'],
  #ap-ssid {
    width: 100%;
    min-width: 0;
    padding: 0.55rem 0.6rem;
    border: 1px solid var(--border);
    border-radius: 0.25rem;
    background: var(--background);
    color: var(--text);
    font: inherit;
  }

  .wifi-connect button,
  .wifi-ap-form > button { margin-top: 0.45rem; }

  .wifi-ap-form fieldset { display: grid; gap: 0.35rem; }
  .switch-row {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    margin: 0.4rem 0 0.2rem;
  }
  .switch-row input { accent-color: var(--accent); }

  .saved-title { margin-top: 1rem; }

  .saved-networks {
    margin-top: 0.45rem;
    border-top: 1px solid var(--border);
  }

  .saved-networks > div {
    min-width: 0;
    padding: 0.45rem 0;
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto auto;
    align-items: center;
    gap: 0.35rem;
    border-bottom: 1px solid var(--border);
  }

  .saved-networks span { overflow-wrap: anywhere; }

  .ap-clients {
    margin: 0.45rem 0 0;
    padding-left: 1.4rem;
    color: var(--muted);
    font: 0.82rem ui-monospace, SFMono-Regular, Menlo, monospace;
  }

  .table-view { max-width: 42rem; }

  form { margin-top: 0.3rem; }

  fieldset {
    min-width: 0;
    margin: 0;
    padding: 0;
    border: 0;
  }

  legend,
  .field-label {
    display: block;
    width: 100%;
    margin: 0.9rem 0 0.38rem;
    color: var(--muted);
    font-size: 0.8rem;
    font-weight: 700;
    text-transform: uppercase;
  }

  fieldset > legend:first-child { margin-top: 0; }

  .form-segments {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    overflow: hidden;
    border: 1px solid var(--border);
    border-radius: 0.35rem;
  }

  .form-segments button {
    min-height: 2.25rem;
    border: 0;
    border-left: 1px solid var(--border);
    color: var(--text);
  }

  .form-segments button:first-child { border-left: 0; }

  .field-note {
    margin-top: 0.35rem;
    color: var(--muted);
    font-size: 0.78rem;
  }

  #hostname {
    width: 100%;
    padding: 0.55rem 0.6rem;
    border: 1px solid var(--border);
    border-radius: 0.25rem;
    background: var(--surface);
    color: var(--text);
    font: inherit;
  }

  .reset-options {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.5rem 1rem;
  }

  .reset-options label {
    display: flex;
    align-items: center;
    gap: 0.45rem;
  }

  .reset-options input { accent-color: var(--accent); }

  .save-warning {
    margin-top: 1rem;
    color: var(--warning);
    font-size: 0.82rem;
  }

  .success {
    margin: 0.65rem 0;
    color: var(--charge);
  }

  .form-actions {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.5rem;
    margin-top: 0.65rem;
  }

  .form-actions button {
    min-height: 2.3rem;
    border-radius: 0.3rem;
  }

  .form-actions .secondary { border-color: var(--border); }
  .form-actions .primary {
    background: var(--accent);
    color: white;
  }

  .striped-details {
    overflow: hidden;
    border: 1px solid var(--border);
    border-radius: 0.35rem;
    gap: 0;
  }

  .striped-details dt,
  .striped-details dd { padding: 0.42rem 0.55rem; }

  .striped-details dt:nth-of-type(even),
  .striped-details dd:nth-of-type(even) { background: var(--surface); }

  .update-card {
    margin-top: 0.75rem;
    padding: 0.8rem;
    border: 1px solid var(--border);
    border-radius: 0.4rem;
    background: var(--surface);
  }

  .info-view {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }

  .info-view .update-card { margin-top: auto; }

  .update-heading,
  .update-actions {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.75rem;
  }

  .update-heading h2 { margin: 0; }

  .update-details {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.3rem 0.8rem;
    margin: 0.75rem 0;
  }

  .update-details dt { color: var(--muted); }
  .update-details dd { margin: 0; }
  .update-card progress { width: 100%; margin: 0.2rem 0 0.7rem; }
  .update-actions { justify-content: flex-start; margin-top: 0.75rem; }
  .update-contact {
    display: flex;
    flex-wrap: wrap;
    gap: 0.2rem 0.45rem;
    margin-top: 0.75rem;
    padding-top: 0.65rem;
    border-top: 1px solid var(--border);
    color: var(--muted);
    font-size: 0.78rem;
  }
  .update-contact a { color: var(--accent); }

  h2 {
    margin: 1rem 0 0.55rem;
    font-size: 1rem;
  }

  h3 {
    margin: 0;
    font-size: 1rem;
  }

  .state {
    display: inline-block;
    color: var(--muted);
    font-size: 0.78rem;
  }

  .state.good { color: var(--charge); }
  .state.warning { color: var(--warning); }
  .state.bad { color: var(--battery); }

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

  .adc-capture-view {
    width: 100%;
    max-width: 64rem;
    margin-left: auto;
    margin-right: auto;
  }

  .adc-capture-toolbar {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    align-items: center;
    gap: 0.8rem;
    margin-bottom: 1rem;
  }

  .adc-capture-toolbar h2 { margin: 0; }
  .adc-capture-toolbar p {
    margin-top: 0.15rem;
    color: var(--muted);
    font-size: 0.8rem;
  }

  .adc-capture-progress {
    min-height: 10rem;
    display: grid;
    place-content: center;
    justify-items: center;
    gap: 0.5rem;
    color: var(--muted);
    font-size: 0.85rem;
  }

  .adc-capture-heading {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 0.8rem;
    margin-bottom: 0.45rem;
  }

  .adc-capture-heading p,
  .adc-capture-heading span {
    margin-top: 0.2rem;
    color: var(--muted);
    font-size: 0.78rem;
  }

  .adc-capture-heading span { white-space: nowrap; }

  .sensor-card-status {
    display: flex;
    align-items: center;
    gap: 0.55rem;
  }

  .adc-window-table-wrap {
    margin-top: 0.5rem;
    overflow-x: auto;
    border: 1px solid var(--border);
    border-radius: 0.3rem;
  }

  .adc-window-table {
    width: 100%;
    border-collapse: collapse;
    font-variant-numeric: tabular-nums;
  }

  .adc-window-table th,
  .adc-window-table td {
    padding: 0.45rem 0.6rem;
    text-align: right;
    white-space: nowrap;
  }

  .adc-window-table th:first-child { text-align: left; }
  .adc-window-table thead { color: var(--muted); font-size: 0.76rem; }
  .adc-window-table tbody tr:nth-child(even) { background: var(--background); }
  .adc-window-table td.warning { color: var(--warning); }
  .adc-window-table td.bad { color: var(--battery); }

  .sensor-tabs {
    display: flex;
    margin-bottom: 0.7rem;
    border: 1px solid var(--border);
    border-radius: 0.35rem;
    overflow: hidden;
  }

  .sensor-tabs button { flex: 1; min-height: 2.3rem; border-left: 1px solid var(--border); }
  .sensor-tabs button:first-child { border-left: 0; }
  .sensor-tabs button.active { color: white; background: var(--accent); }

  .sensor-card {
    padding: 0.75rem;
    border: 1px solid var(--border);
    background: var(--surface);
  }

  .sensor-card.warning { border-color: var(--warning); }
  .sensor-card.bad { border-color: var(--battery); }

  .sensor-panel { display: none; min-width: 0; }
  .sensor-panel.active { display: block; }

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

  .sensor-charts {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 0.8rem;
    margin-top: 0.7rem;
  }

  .calibration-unit-toggle {
    grid-column: 1 / -1;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.7rem;
    color: var(--muted);
    font-size: 0.8rem;
  }

  .calibration-unit-toggle > div {
    display: flex;
    border: 1px solid var(--border);
    border-radius: 0.3rem;
    overflow: hidden;
  }

  .calibration-unit-toggle button {
    min-height: 2rem;
    padding: 0.35rem 0.6rem;
    border-left: 1px solid var(--border);
  }

  .calibration-unit-toggle button:first-child { border-left: 0; }
  .calibration-unit-toggle button.active { color: white; background: var(--accent); }

  .calibration-actions,
  .calibration-editor-actions {
    display: flex;
    flex-wrap: wrap;
    justify-content: flex-end;
    gap: 0.45rem;
    margin-top: 0.65rem;
  }

  .calibration-actions button,
  .calibration-editor-actions button,
  .calibration-reference button {
    min-height: 2.2rem;
    padding: 0.4rem 0.65rem;
    border-color: var(--border);
    border-radius: 0.3rem;
    color: var(--text);
  }

  .calibration-editor {
    margin-top: 0.8rem;
    padding: 0.8rem;
    border: 1px solid var(--border);
    border-radius: 0.35rem;
    background: var(--surface);
  }

  .calibration-editor > label,
  .calibration-reference label {
    display: grid;
    gap: 0.25rem;
    margin-top: 0.6rem;
    color: var(--muted);
    font-size: 0.8rem;
  }

  .calibration-editor input {
    width: 100%;
    padding: 0.48rem;
    border: 1px solid var(--border);
    border-radius: 0.25rem;
    background: var(--background);
    color: var(--text);
    font: inherit;
  }

  .calibration-reference {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    align-items: end;
    gap: 0.5rem;
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

  @keyframes reconnect-pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.25; }
  }

  @media (prefers-reduced-motion: reduce) {
    .connection.reconnecting b { animation: none; }
  }

  @media (min-width: 64rem) {
    .sensors-view { max-width: none; }
    .sensor-tabs { display: none; }
    .sensor-grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      align-items: start;
      gap: 1rem;
    }
    .sensor-grid.calibrating {
      grid-template-columns: minmax(0, 2fr) minmax(18rem, 1fr);
    }
    .sensor-grid.calibrating .calibration-editor {
      align-self: start;
      margin-top: 0.7rem;
    }
    .sensor-panel,
    .sensor-panel.active { display: block; }
    .sensor-charts { grid-template-columns: 1fr; }
  }

  @media (max-width: 34rem) {
    main {
      padding-left: 0.5rem;
      padding-right: 0.5rem;
    }

    .main-nav {
      margin-left: -0.5rem;
      margin-right: -0.5rem;
      padding-left: 0.5rem;
      padding-right: 0.5rem;
    }

    .remote {
      height: min(calc(100dvh - 6.7rem), 480px);
    }

    .sensor-charts { grid-template-columns: 1fr; }
    .sensor-heading { align-items: flex-start; }
    .adc-capture-heading { display: block; }
    .adc-capture-toolbar {
      grid-template-columns: minmax(0, 1fr) auto;
      align-items: start;
    }
    .adc-capture-toolbar .back-button {
      grid-column: 1 / -1;
      justify-self: start;
    }

    .wifi-grid { grid-template-columns: 1fr; }
  }
</style>
