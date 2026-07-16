export async function getStatus() {
  const response = await fetch('/api/v1/web/status', { cache: 'no-store' });
  if (!response.ok) throw new Error(`status ${response.status}`);
  return response.json();
}

export async function getSensors() {
  const response = await fetch('/api/v1/sensors', { cache: 'no-store' });
  if (!response.ok) throw new Error(`sensors ${response.status}`);
  return response.json();
}

export async function getSetup() {
  const response = await fetch('/api/v1/setup', { cache: 'no-store' });
  if (!response.ok) throw new Error(`setup ${response.status}`);
  return response.json();
}

export async function saveSetup(settings) {
  const response = await fetch('/api/v1/setup', {
    method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(settings)
  });
  const result = await response.json().catch(() => ({}));
  if (!response.ok) {
    const error = new Error(result.error || `setup ${response.status}`);
    error.status = response.status;
    error.userMessage = result.error;
    throw error;
  }
  return result;
}

export async function getDebug() {
  const response = await fetch('/api/v1/debug', { cache: 'no-store' });
  if (!response.ok) throw new Error(`debug ${response.status}`);
  return response.json();
}

export async function anchorTime() {
  const response = await fetch('/api/v1/time/anchor', {
    method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ unix_ms: Date.now(), utc_offset_minutes: -new Date().getTimezoneOffset() })
  });
  return response.ok;
}

export async function getRemoteScreenshot() {
  const response = await fetch('/api/v1/display/screenshot.bmp', { cache: 'no-store' });
  if (!response.ok) throw new Error(`remote display ${response.status}`);
  return URL.createObjectURL(await response.blob());
}

export async function sendRemotePointer(x, y, pressed) {
  const response = await fetch('/api/v1/display/pointer', {
    method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ x, y, pressed })
  });
  if (!response.ok) throw new Error(`remote control ${response.status}`);
}

export function openLiveSocket(onFrame, onState) {
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const socket = new WebSocket(`${protocol}//${location.hostname}:81/api/v1/live`);
  let connectionLimit = 0;
  socket.binaryType = 'arraybuffer';
  socket.onopen = () => onState?.('live');
  socket.onerror = () => onState?.('reconnecting');
  socket.onmessage = (event) => {
    if (typeof event.data === 'string') {
      const match = /^limit:(\d+)$/.exec(event.data);
      if (match) { connectionLimit = Number(match[1]); onState?.('limited', connectionLimit); }
      return;
    }
    if (!(event.data instanceof ArrayBuffer)) return;
    const view = new DataView(event.data);
    if (view.byteLength !== 64 || view.getUint32(0, true) !== 0x324d5056 ||
        view.getUint8(4) !== 2 || view.getUint8(5) !== 1 || view.getUint32(20, true) === 0) return;
    const flags = view.getUint16(6, true);
    onFrame({
      sequence: view.getUint32(8, true),
      stateRevision: view.getUint32(12, true),
      uptimeMs: view.getUint32(16, true),
      sessionId: view.getUint32(20, true),
      unixMs: view.getFloat64(24, true),
      configuredMask: (flags >> 1) & 0x07,
      eligibleMask: (flags >> 4) & 0x07,
      in: { voltage: view.getFloat32(32, true), current: view.getFloat32(36, true), power: view.getFloat32(40, true) },
      out: { voltage: view.getFloat32(44, true), current: view.getFloat32(48, true), power: view.getFloat32(52, true) },
      auxPower: view.getFloat32(56, true),
      netBatteryPower: view.getFloat32(60, true)
    });
  };
  socket.onclose = () => onState?.(connectionLimit ? 'limited' : 'offline', connectionLimit);
  return socket;
}

function parseHistory(buffer) {
  const view = new DataView(buffer);
  if (view.byteLength < 32 || view.getUint32(0, true) !== 0x32485056 || view.getUint8(4) !== 2 || view.getUint8(5) !== 2) throw new Error('Unsupported history response');
  const flags = view.getUint16(6, true), count = view.getUint16(8, true), recordBytes = view.getUint16(10, true);
  if (recordBytes !== 80 || view.byteLength !== 32 + count * recordBytes) throw new Error('Invalid history response');
  const startUnixMs = view.getFloat64(16, true);
  const endUnixMs = view.getFloat64(24, true);
  const buckets = [];
  for (let index = 0; index < count; index += 1) {
    const offset = 32 + index * recordBytes;
    const coveredMs = view.getUint32(offset + 8, true);
    const channelCoverageMs = Array.from({ length: 3 }, (_, value) => view.getUint32(offset + 48 + value * 4, true));
    const componentCoverageMs = Array.from({ length: 5 }, (_, value) => view.getUint32(offset + 60 + value * 4, true));
    const watts = (value, coverage) => coverage ? value * 3600000 / coverage : Number.NaN;
    buckets.push({
      unixMs: view.getFloat64(offset, true), coveredMs,
      configuredChannelMask: view.getUint8(offset + 12), timeFlags: view.getUint8(offset + 13),
      qualityFlags: view.getUint8(offset + 14), channelCoverageMs, componentCoverageMs,
      in: watts(view.getFloat32(offset + 16, true), channelCoverageMs[0]),
      out: watts(view.getFloat32(offset + 20, true), channelCoverageMs[1]),
      aux: watts(view.getFloat32(offset + 24, true), channelCoverageMs[2]),
      charging: watts(view.getFloat32(offset + 28, true), componentCoverageMs[0]),
      batteryUsage: watts(view.getFloat32(offset + 32, true), componentCoverageMs[1]),
      panelIn: watts(view.getFloat32(offset + 36, true), componentCoverageMs[2]),
      panelUsage: watts(view.getFloat32(offset + 40, true), componentCoverageMs[3]),
      panelSurplus: watts(view.getFloat32(offset + 44, true), componentCoverageMs[4])
    });
  }
  const bucketMinutes = count > 1
    ? Math.max(1, Math.round((buckets[1].unixMs - buckets[0].unixMs) / 60_000))
    : (count === 1 && endUnixMs > startUnixMs
      ? Math.max(1, Math.round((endUnixMs - startUnixMs) / 60_000))
      : 0);
  return { flags, startUnixMs, endUnixMs, bucketMinutes, buckets };
}

export async function getCycles() {
  const start = await fetch('/api/v1/cycles', { cache: 'no-store' });
  if (start.status !== 202) throw new Error(`cycles ${start.status}`);
  const { job } = await start.json();
  for (let attempt = 0; attempt < 80; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 250));
    const response = await fetch(`/api/v1/cycles?job=${job}`, { cache: 'no-store' });
    if (response.status === 202) continue;
    if (!response.ok) throw new Error(`cycles ${response.status}`);
    return response.json();
  }
  throw new Error('cycle query timed out');
}

export async function saveCycleEndHour(endHour) {
  const response = await fetch('/api/v1/cycles', {
    method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ end_hour: endHour })
  });
  const result = await response.json().catch(() => ({}));
  if (!response.ok) {
    const error = new Error(result.error || `cycles ${response.status}`);
    error.status = response.status;
    error.userMessage = result.error;
    throw error;
  }
  return result;
}

export async function getHistory(range = 'today', bucketMinutes = 30) {
  const params = new URLSearchParams({ range, bucket_minutes: String(bucketMinutes) });
  const start = await fetch(`/api/v1/history/query?${params}`, { cache: 'no-store' });
  if (start.status !== 202) throw new Error(`history ${start.status}`);
  const { job } = await start.json();
  for (let attempt = 0; attempt < 80; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 250));
    const response = await fetch(`/api/v1/history/query?job=${job}`, { cache: 'no-store' });
    if (response.status === 202) continue;
    if (!response.ok) throw new Error(`history ${response.status}`);
    return parseHistory(await response.arrayBuffer());
  }
  throw new Error('history query timed out');
}
