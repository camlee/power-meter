<script>
  import { onMount } from 'svelte';
  import { projectLiveNow } from './liveTime.js';

  export let points = [];
  export let sessionId = null;
  export let active = true;

  const WINDOW_OPTIONS = [
    { seconds: 30, tickSeconds: 10, label: '30 seconds' },
    { seconds: 60, tickSeconds: 20, label: '1 minute' },
    { seconds: 120, tickSeconds: 30, label: '2 minutes' },
    { seconds: 300, tickSeconds: 60, label: '5 minutes' },
  ];
  const DEFAULT_WINDOW_SECONDS = 30;
  const GAP_THRESHOLD_MS = 3_000;
  const CHART_PADDING = { left: 47, right: 8, top: 12, bottom: 29 };

  // A delayed replay must never pull the visible window backward. We only
  // resync when device time has moved materially *ahead* of our extrapolated
  // clock; ordinary jitter and reconnect backlog remain at their real x
  // positions and slide naturally into view.
  const MAX_FORWARD_CLOCK_DRIFT_MS = GAP_THRESHOLD_MS;

  // Small filled circle drawn at each data point, on top of the lines.
  const MARKER_RADIUS = 2.5;

  let canvas;
  let windowSeconds = DEFAULT_WINDOW_SECONDS;
  let rafId = null;
  let referenceDataTime = null;
  let referenceWallTime = null;
  let referenceMonotonicTime = null;
  let referenceSessionId = null;
  let sortedPoints = [];
  let chartWidth = 0;
  let chartHeight = 0;
  let colors = null;

  $: windowOption = WINDOW_OPTIONS.find((option) => option.seconds === windowSeconds) ?? WINDOW_OPTIONS[0];

  // Defensive: `points` is expected to arrive in ascending timestamp order,
  // but a reconnect backlog merge or a batch of updates flushed after the
  // tab was hidden can land a point out of order (e.g. a stale point
  // re-appended after fresher ones). extendedPoints()'s window-slice scan and
  // drawSeries()'s point-to-point line drawing assume ascending order —
  // without this guard, one out-of-order point makes rendered lines
  // visibly jump backward, and can make legitimately in-window points
  // fall outside the slice extendedPoints() computes (i.e. data appears
  // to vanish from the chart).
  $: sortedPoints = sortPoints(points);

  // A new firmware session is the one event where moving to a completely
  // different timestamp domain is expected. Reset the chart clock along
  // with App.svelte's point buffer instead of trying to infer a reboot from
  // uptime or timestamp movement.
  $: if (sessionId !== referenceSessionId) {
    referenceSessionId = sessionId;
    referenceDataTime = null;
    referenceWallTime = null;
    referenceMonotonicTime = null;
  }

  function sortPoints(list) {
    return list
      .filter((point) => point && Number.isFinite(point.timestamp))
      .slice()
      .sort((a, b) => a.timestamp - b.timestamp);
  }

  function newestReceivedPoint() {
    for (let index = points.length - 1; index >= 0; index -= 1) {
      if (points[index] && Number.isFinite(points[index].timestamp)) return points[index];
    }
    return null;
  }

  function palette() {
    const css = getComputedStyle(document.documentElement);
    return {
      grid: css.getPropertyValue('--chart-grid').trim(),
      zero: css.getPropertyValue('--chart-zero').trim(),
      muted: css.getPropertyValue('--muted').trim(),
      charge: css.getPropertyValue('--charge').trim(),
      battery: css.getPropertyValue('--battery').trim(),
    };
  }

  function niceStep(span) {
    const power = 10 ** Math.floor(Math.log10(Math.max(span, 1)));
    const normalized = span / power;
    const base = normalized <= 1.5 ? 1 : normalized <= 3 ? 2 : normalized <= 7 ? 5 : 10;
    return base * power;
  }

  // We do NOT reset this reference every time a new point arrives. Doing so
  // would force each point to render exactly at the current right edge
  // (windowEnd IS virtualNow),
  // which is what causes points to pop into place instead of sliding in.
  // Leaving the reference alone for small drift means a point that
  // arrives a little ahead of or behind our prediction still renders at
  // its own real timestamp — possibly briefly beyond the (clipped) right
  // edge — and eases into view over the next frames as virtualNow's
  // normal wall-clock extrapolation catches up to it.
  function virtualNow() {
    const last = newestReceivedPoint();
    const wallNow = Date.now();
    const monotonicNow = performance.now();
    if (last && Number.isFinite(last.timestamp)) {
      if (referenceDataTime == null) {
        referenceDataTime = projectLiveNow(last, { wallNow, monotonicNow });
        referenceWallTime = wallNow;
        referenceMonotonicTime = monotonicNow;
      } else if (last.timestamp !== referenceDataTime) {
        const predicted = referenceDataTime + (monotonicNow - referenceMonotonicTime);
        const projected = projectLiveNow(last, { wallNow, monotonicNow });
        const drift = projected - predicted;
        if (drift >= MAX_FORWARD_CLOCK_DRIFT_MS) {
          // A forward clock correction should become visible promptly. A
          // negative drift is commonly delayed replay and intentionally
          // does not move the window backward.
          referenceDataTime = projected;
          referenceWallTime = wallNow;
          referenceMonotonicTime = monotonicNow;
        }
      }
    }
    return referenceDataTime != null
      ? referenceDataTime + (monotonicNow - referenceMonotonicTime)
      : wallNow;
  }

  function wallClockOffset() {
    return referenceDataTime != null ? referenceWallTime - referenceDataTime : 0;
  }

  function computeYAxis(visiblePoints) {
    const values = visiblePoints.flatMap((point) => [point.in, point.out]).filter(Number.isFinite);
    const low = Math.min(0, ...values);
    const high = Math.max(1, ...values);
    const tick = niceStep((high - low || 1) / 4);
    return {
      low: Math.floor(low / tick) * tick,
      high: Math.ceil(high / tick) * tick,
      tick,
    };
  }

  function drawYAxis(ctx, plot, axis, colors) {
    const scaleY = (value) => plot.top + ((axis.high - value) * plot.height) / (axis.high - axis.low);

    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';

    for (let value = axis.low; value <= axis.high + axis.tick * 0.01; value += axis.tick) {
      const y = scaleY(value);
      ctx.strokeStyle = value === 0 ? colors.zero : colors.grid;
      ctx.lineWidth = value === 0 ? 1.25 : 1;
      ctx.beginPath();
      ctx.moveTo(plot.left, y);
      ctx.lineTo(plot.left + plot.width, y);
      ctx.stroke();

      ctx.fillStyle = colors.muted;
      ctx.fillText(`${Math.round(value)} W`, plot.left - 5, y);
    }

    return scaleY;
  }

  const TIME_FORMAT_WITH_SECONDS = new Intl.DateTimeFormat(undefined, {
    hour12: true, hour: 'numeric', minute: '2-digit', second: '2-digit'
  });
  const TIME_FORMAT_MINUTES_ONLY = new Intl.DateTimeFormat(undefined, {
    hour12: true, hour: 'numeric', minute: '2-digit',
  });

  function drawXAxis(ctx, plot, windowStart, windowEnd, tickSeconds, colors) {
    const offset = wallClockOffset();
    const tickMs = tickSeconds * 1000;
    const majorTickMs = windowEnd - windowStart <= 30_000 ? 30_000 : 60_000;
    const wallStart = windowStart + offset;
    const wallEnd = windowEnd + offset;
    const firstTickWall = Math.ceil(wallStart / tickMs) * tickMs;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';

    for (let wallTick = firstTickWall; wallTick <= wallEnd + tickMs * 0.01; wallTick += tickMs) {
      const dataTimestamp = wallTick - offset;
      const fraction = (dataTimestamp - windowStart) / (windowEnd - windowStart);
      const x = plot.left + plot.width * fraction;

      ctx.strokeStyle = colors.grid;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, plot.top);
      ctx.lineTo(x, plot.top + plot.height);
      ctx.stroke();

      const isMajorTick = wallTick % majorTickMs === 0;
      if (isMajorTick) {
        const formatter = wallTick % 60_000 === 0 ? TIME_FORMAT_MINUTES_ONLY : TIME_FORMAT_WITH_SECONDS;
        const label = formatter.format(new Date(wallTick));
        const halfLabelWidth = ctx.measureText(label).width / 2;
        const labelX = Math.max(
          plot.left + halfLabelWidth,
          Math.min(plot.left + plot.width - halfLabelWidth, x),
        );
        ctx.fillStyle = colors.muted;
        ctx.fillText(label, labelX, plot.top + plot.height + 8);
      }
    }
  }

  // `visiblePoints` (strictly inside [windowStart, windowEnd]) is what we
  // use for axis scaling and the "no readings" empty state — that should
  // only reflect what's actually in the window.
  //
  // For DRAWING the lines, though, strictly-inside points cause a visible
  // pop: a segment whose one endpoint is a hair outside the window gets
  // dropped entirely, so a point appears/disappears abruptly right at the
  // edge instead of sliding off it. To fix that we draw with one extra
  // point tacked on each side — the last known point before windowStart
  // and the first known point after windowEnd, if they exist — and rely
  // on canvas clipping (see draw()) to cut those segments off cleanly at
  // the plot boundary. `drawSeries`'s existing gap logic still applies to
  // these extra points, so a real connection gap straddling the edge
  // still renders as a break rather than a fake connecting line.
  //
  // This scan assumes ascending timestamp order, which is why it reads
  // from `sortedPoints` rather than the raw `points` prop — see
  // sortPoints() above.
  function extendedPoints(windowStart, windowEnd) {
    let startIndex = sortedPoints.findIndex((point) => point.timestamp >= windowStart);
    if (startIndex === -1) startIndex = sortedPoints.length;

    let endIndex = sortedPoints.length - 1;
    for (let i = startIndex; i < sortedPoints.length; i++) {
      if (sortedPoints[i].timestamp > windowEnd) {
        endIndex = i;
        break;
      }
    }

    const from = startIndex > 0 ? startIndex - 1 : 0;
    return sortedPoints.slice(from, endIndex + 1);
  }

  function drawSeries(ctx, visiblePoints, field, color, scaleX, scaleY) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.beginPath();

    let previousTimestamp = null;
    visiblePoints.forEach((point) => {
      const value = point[field];
      if (!Number.isFinite(value)) {
        previousTimestamp = null;
        return;
      }
      const isGap = previousTimestamp != null && point.timestamp - previousTimestamp > GAP_THRESHOLD_MS;
      if (previousTimestamp == null || isGap) {
        ctx.moveTo(scaleX(point.timestamp), scaleY(value));
      } else {
        ctx.lineTo(scaleX(point.timestamp), scaleY(value));
      }
      previousTimestamp = point.timestamp;
    });

    ctx.stroke();
  }

  // Small filled circle at each data point, drawn on top of the line.
  function drawMarkers(ctx, dataPoints, field, color, scaleX, scaleY) {
    ctx.fillStyle = color;
    dataPoints.forEach((point) => {
      const value = point[field];
      if (!Number.isFinite(value)) return;
      ctx.beginPath();
      ctx.arc(scaleX(point.timestamp), scaleY(value), MARKER_RADIUS, 0, Math.PI * 2);
      ctx.fill();
    });
  }

  function draw() {
    if (!canvas || chartWidth <= 0 || chartHeight <= 0) return;

    const ratio = window.devicePixelRatio || 1;
    const pixelWidth = Math.max(1, Math.round(chartWidth * ratio));
    const pixelHeight = Math.max(1, Math.round(chartHeight * ratio));
    if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
    if (canvas.height !== pixelHeight) canvas.height = pixelHeight;

    const ctx = canvas.getContext('2d');
    const width = chartWidth;
    const height = chartHeight;
    const plot = {
      left: CHART_PADDING.left,
      top: CHART_PADDING.top,
      width: width - CHART_PADDING.left - CHART_PADDING.right,
      height: height - CHART_PADDING.top - CHART_PADDING.bottom,
    };
    const chartColors = colors ?? (colors = palette());

    // setTransform avoids accumulating scale when the backing dimensions did
    // not change. Resizing the canvas is intentionally limited to real size
    // or device-pixel-ratio changes because it reallocates the backing store.
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.font = '11px system-ui, sans-serif';

    const windowMs = windowOption.seconds * 1000;
    const windowEnd = virtualNow();
    const windowStart = windowEnd - windowMs;
    const visiblePoints = sortedPoints.filter(
      (point) => point.timestamp >= windowStart && point.timestamp <= windowEnd,
    );

    const axis = computeYAxis(visiblePoints);
    const scaleY = drawYAxis(ctx, plot, axis, chartColors);
    drawXAxis(ctx, plot, windowStart, windowEnd, windowOption.tickSeconds, chartColors);

    const scaleX = (timestamp) => plot.left + (plot.width * (timestamp - windowStart)) / windowMs;

    if (!sortedPoints.length) {
      ctx.textAlign = 'center';
      ctx.fillStyle = chartColors.muted;
      ctx.fillText('Waiting for live readings…', width / 2, height / 2);
      return;
    }

    if (!visiblePoints.length) {
      ctx.textAlign = 'center';
      ctx.fillStyle = chartColors.muted;
      ctx.fillText(`No readings in the last ${windowOption.label}`, width / 2, height / 2);
      return;
    }

    const drawPoints = extendedPoints(windowStart, windowEnd);

    ctx.save();
    ctx.beginPath();
    ctx.rect(plot.left, plot.top, plot.width, plot.height);
    ctx.clip();
    drawSeries(ctx, drawPoints, 'in', chartColors.charge, scaleX, scaleY);
    drawMarkers(ctx, drawPoints, 'in', chartColors.charge, scaleX, scaleY);
    drawSeries(ctx, drawPoints, 'out', chartColors.battery, scaleX, scaleY);
    drawMarkers(ctx, drawPoints, 'out', chartColors.battery, scaleX, scaleY);
    ctx.restore();
  }

  function loop() {
    draw();
    rafId = active ? requestAnimationFrame(loop) : null;
  }

  function ensureLoopRunning() {
    if (active && rafId == null) {
      rafId = requestAnimationFrame(loop);
    }
  }

  $: sortedPoints, sessionId, active, windowOption, draw();
  $: if (active) ensureLoopRunning();

  onMount(() => {
    const resize = () => {
      const rect = canvas.getBoundingClientRect();
      chartWidth = rect.width;
      chartHeight = rect.height;
      draw();
    };
    const refreshColors = () => {
      colors = palette();
      draw();
    };
    const colorScheme = window.matchMedia('(prefers-color-scheme: dark)');
    const observer = new ResizeObserver(resize);
    observer.observe(canvas);
    colorScheme.addEventListener('change', refreshColors);
    window.addEventListener('viewe-theme-change', refreshColors);
    refreshColors();
    resize();
    ensureLoopRunning();

    return () => {
      observer.disconnect();
      colorScheme.removeEventListener('change', refreshColors);
      window.removeEventListener('viewe-theme-change', refreshColors);
      if (rafId != null) cancelAnimationFrame(rafId);
      rafId = null;
    };
  });
</script>

<div class="live-chart">
  <div class="window-control">
    <label class="sr-only" for="live-window">Time window</label>
    <select id="live-window" bind:value={windowSeconds}>
      {#each WINDOW_OPTIONS as option}
        <option value={option.seconds}>{option.label}</option>
      {/each}
    </select>
  </div>
  <canvas bind:this={canvas} aria-label="Live In and Out power graph with watt and time axes"></canvas>
</div>

<style>
  .live-chart {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    min-height: 0;
  }

  .window-control {
    display: flex;
    justify-content: flex-end;
  }

  .window-control select {
    border: 1px solid var(--border);
    border-radius: 0;
    padding: 0.3rem 1.6rem 0.3rem 0.5rem;
    background: var(--background);
    color: var(--text);
    font: 600 0.8rem system-ui, sans-serif;
  }

  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
  }

  canvas {
    flex: 1;
    width: 100%;
    height: auto;
    min-height: 14rem;
    display: block;
  }
</style>
