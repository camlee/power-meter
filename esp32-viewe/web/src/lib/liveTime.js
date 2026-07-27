function browserMonotonicNow() {
  return globalThis.performance?.now?.();
}

// Projects the device timestamp of the newest point forward to the browser's
// current instant. Prefer a monotonic browser receipt timestamp so a wall-clock
// correction cannot make live data jump backward or become fresh again.
export function projectLiveNow(point, {
  wallNow = Date.now(),
  monotonicNow = browserMonotonicNow(),
} = {}) {
  if (!Number.isFinite(point?.timestamp)) return wallNow;

  let elapsed = 0;
  if (Number.isFinite(point.receivedMonotonicAt) && Number.isFinite(monotonicNow)) {
    elapsed = monotonicNow - point.receivedMonotonicAt;
  } else if (Number.isFinite(point.receivedAt)) {
    elapsed = wallNow - point.receivedAt;
  }

  return point.timestamp + Math.max(0, elapsed);
}
