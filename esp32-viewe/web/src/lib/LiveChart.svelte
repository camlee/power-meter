<script>
  import { onMount } from 'svelte';

  export let points = [];

  // ---------------------------------------------------------------------
  // Constants
  // ---------------------------------------------------------------------

  // Selectable time windows. `tickSeconds` is a round, human-friendly
  // increment for that window (never computed dynamically, so ticks are
  // always on a clean boundary like 5s/10s/30s/1m).
  const WINDOW_OPTIONS = [
    { seconds: 30, tickSeconds: 5, label: '30 seconds' },
    { seconds: 60, tickSeconds: 10, label: '1 minute' },
    { seconds: 120, tickSeconds: 30, label: '2 minutes' },
    { seconds: 300, tickSeconds: 60, label: '5 minutes' },
  ];
  const DEFAULT_WINDOW_SECONDS = 30;

  // If two consecutive visible points are further apart than this, treat
  // it as a connection gap (e.g. a websocket reconnect) rather than a
  // continuous reading, and don't draw a line across it.
  const GAP_THRESHOLD_MS = 3_000;

  // How often to redraw on a timer, independent of new data arriving.
  // This is what makes the axis keep scrolling — and old points age off
  // the left edge, and a stalled connection visibly goes blank — even
  // when no new frames are coming in.
  const REDRAW_INTERVAL_MS = 500;

  const CHART_PADDING = { left: 47, right: 8, top: 12, bottom: 29 };

  // ---------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------

  let canvas;
  let windowSeconds = DEFAULT_WINDOW_SECONDS;
  let redrawTimer;

  // Used to extrapolate "now" in the same clock domain as point
  // timestamps (see virtualNow() below).
  let referenceDataTime = null;
  let referenceWallTime = null;

  $: windowOption = WINDOW_OPTIONS.find((option) => option.seconds === windowSeconds) ?? WINDOW_OPTIONS[0];

  // ---------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------

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

  // Nice round step for the y-axis (watts), e.g. 1/2/5/10/20/50/100...
  function niceStep(span) {
    const power = 10 ** Math.floor(Math.log10(Math.max(span, 1)));
    const normalized = span / power;
    const base = normalized <= 1.5 ? 1 : normalized <= 3 ? 2 : normalized <= 7 ? 5 : 10;
    return base * power;
  }

  // Point timestamps may be either wall-clock (unix ms) or device uptime
  // (ms since boot) — we don't know which, and it can vary by whether the
  // meter's clock is time-synced. Either way they advance at the same
  // rate as real time, so instead of assuming Date.now() lines up with
  // them, we track the offset between "the last timestamp we saw" and
  // "the wall-clock time we saw it", and extrapolate forward from there.
  // This keeps the chart scrolling smoothly and correctly even if the
  // point timestamps are nowhere near Date.now() in absolute terms.
  function virtualNow() {
    const last = points[points.length - 1];
    if (last && Number.isFinite(last.timestamp)) {
      if (referenceDataTime !== last.timestamp) {
        referenceDataTime = last.timestamp;
        referenceWallTime = Date.now();
      }
      return referenceDataTime + (Date.now() - referenceWallTime);
    }
    return referenceDataTime != null ? referenceDataTime + (Date.now() - referenceWallTime) : Date.now();
  }

  function formatTickLabel(secondsAgo) {
    if (secondsAgo === 0) return 'now';
    if (secondsAgo % 60 === 0) return `-${secondsAgo / 60}m`;
    return `-${secondsAgo}s`;
  }

  // ---------------------------------------------------------------------
  // Drawing
  // ---------------------------------------------------------------------

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

  function drawXAxis(ctx, plot, windowMs, tickSeconds, colors) {
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';

    const tickMs = tickSeconds * 1000;
    for (let elapsed = 0; elapsed <= windowMs + tickMs * 0.01; elapsed += tickMs) {
      // elapsed = 0 is the oldest edge of the window; the axis reads
      // left (oldest) to right (now).
      const fraction = elapsed / windowMs;
      const x = plot.left + plot.width * fraction;

      ctx.strokeStyle = colors.grid;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, plot.top);
      ctx.lineTo(x, plot.top + plot.height);
      ctx.stroke();

      const secondsAgo = Math.round((windowMs - elapsed) / 1000);
      ctx.fillStyle = colors.muted;
      ctx.fillText(formatTickLabel(secondsAgo), x, plot.top + plot.height + 8);
    }
  }

  // Draws one series (in/out), starting a new sub-path — i.e. leaving a
  // visible gap — wherever consecutive points are further apart than
  // GAP_THRESHOLD_MS, so a connection drop doesn't get drawn as a smooth
  // line across the missing time.
  function drawSeries(ctx, visiblePoints, field, color, scaleX, scaleY) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.beginPath();

    let previousTimestamp = null;
    visiblePoints.forEach((point) => {
      const isGap = previousTimestamp != null && point.timestamp - previousTimestamp > GAP_THRESHOLD_MS;
      if (previousTimestamp == null || isGap) {
        ctx.moveTo(scaleX(point.timestamp), scaleY(point[field]));
      } else {
        ctx.lineTo(scaleX(point.timestamp), scaleY(point[field]));
      }
      previousTimestamp = point.timestamp;
    });

    ctx.stroke();
  }

  function draw() {
    if (!canvas) return;

    const rect = canvas.getBoundingClientRect();
    const ratio = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(rect.width * ratio));
    canvas.height = Math.max(1, Math.round(rect.height * ratio));

    const ctx = canvas.getContext('2d');
    const width = rect.width;
    const height = rect.height;
    const plot = {
      left: CHART_PADDING.left,
      top: CHART_PADDING.top,
      width: width - CHART_PADDING.left - CHART_PADDING.right,
      height: height - CHART_PADDING.top - CHART_PADDING.bottom,
    };
    const colors = palette();

    ctx.scale(ratio, ratio);
    ctx.clearRect(0, 0, width, height);
    ctx.font = '11px system-ui, sans-serif';

    // Fixed time window ending "now" — the axis always spans the full
    // selected window, even if we don't have that much data yet, rather
    // than shrinking to fit whatever points happen to be available.
    const windowMs = windowOption.seconds * 1000;
    const now = virtualNow();
    const windowStart = now - windowMs;
    const visiblePoints = points.filter(
      (point) => Number.isFinite(point.timestamp) && point.timestamp >= windowStart && point.timestamp <= now,
    );

    const axis = computeYAxis(visiblePoints);
    const scaleY = drawYAxis(ctx, plot, axis, colors);
    drawXAxis(ctx, plot, windowMs, windowOption.tickSeconds, colors);

    const scaleX = (timestamp) => plot.left + (plot.width * (timestamp - windowStart)) / windowMs;

    if (!points.length) {
      ctx.textAlign = 'center';
      ctx.fillStyle = colors.muted;
      ctx.fillText('Waiting for live readings…', width / 2, height / 2);
      return;
    }

    if (!visiblePoints.length) {
      ctx.textAlign = 'center';
      ctx.fillStyle = colors.muted;
      ctx.fillText(`No readings in the last ${windowOption.label}`, width / 2, height / 2);
      return;
    }

    drawSeries(ctx, visiblePoints, 'in', colors.charge, scaleX, scaleY);
    drawSeries(ctx, visiblePoints, 'out', colors.battery, scaleX, scaleY);
  }

  // ---------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------

  $: points, windowOption, draw();

  onMount(() => {
    const observer = new ResizeObserver(draw);
    observer.observe(canvas);

    // Keep scrolling / age out stale data even when no new points arrive.
    redrawTimer = setInterval(draw, REDRAW_INTERVAL_MS);

    return () => {
      observer.disconnect();
      clearInterval(redrawTimer);
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
