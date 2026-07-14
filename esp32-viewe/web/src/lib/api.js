export async function getStatus() {
  const response = await fetch('/api/v1/web/status', { cache: 'no-store' });
  if (!response.ok) throw new Error(`status ${response.status}`);
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
    if (view.byteLength !== 64 || view.getUint32(0, true) !== 0x314d5056 || view.getUint8(4) !== 1) return;
    onFrame({
      sequence: view.getUint32(8, true),
      stateRevision: view.getUint32(12, true),
      uptimeMs: view.getUint32(16, true),
      unixMs: view.getFloat64(24, true),
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
  if (view.byteLength < 32 || view.getUint32(0, true) !== 0x31485056 || view.getUint8(4) !== 1 || view.getUint8(5) !== 2) throw new Error('Unsupported history response');
  const flags = view.getUint16(6, true), count = view.getUint16(8, true), recordBytes = view.getUint16(10, true);
  if (recordBytes !== 48 || view.byteLength !== 32 + count * recordBytes) throw new Error('Invalid history response');
  const buckets = [];
  for (let index = 0; index < count; index += 1) {
    const offset = 32 + index * recordBytes;
    const coveredMs = view.getUint32(offset + 8, true);
    const watts = (value) => coveredMs ? value * 3600000 / coveredMs : 0;
    buckets.push({
      unixMs: view.getFloat64(offset, true), coveredMs, flags: view.getUint8(offset + 12),
      in: watts(view.getFloat32(offset + 16, true)), out: watts(view.getFloat32(offset + 20, true)),
      aux: watts(view.getFloat32(offset + 24, true)),
      charging: watts(view.getFloat32(offset + 28, true)), batteryUsage: watts(view.getFloat32(offset + 32, true)),
      panelIn: watts(view.getFloat32(offset + 36, true)), panelUsage: watts(view.getFloat32(offset + 40, true)), panelSurplus: watts(view.getFloat32(offset + 44, true))
    });
  }
  return { flags, buckets };
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
