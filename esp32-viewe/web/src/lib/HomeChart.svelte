<script>
  import { onMount } from 'svelte';
  import { projectLiveNow } from './liveTime.js';

  export let points = [];
  export let sessionId = null;
  export let active = true;

  const WINDOW_MS = 30_000;
  const GAP_MS = 3_000;
  const PADDING = { left: 48, right: 8, top: 16, bottom: 28 };
  const THRESHOLDS = [-50, -25, 0, 25, 50];

  let canvas;
  let width = 0;
  let height = 0;
  let rafId = null;
  let referenceSession = null;
  let referenceDataTime = null;
  let referenceMonotonicTime = null;
  let colors = null;
  $: sorted = points.filter((point) => Number.isFinite(point?.timestamp))
    .slice().sort((a, b) => a.timestamp - b.timestamp);
  $: if (sessionId !== referenceSession) {
    referenceSession = sessionId;
    referenceDataTime = null;
    referenceMonotonicTime = null;
  }

  function palette() {
    const css = getComputedStyle(document.documentElement);
    return {
      grid: css.getPropertyValue('--chart-grid').trim(),
      zero: css.getPropertyValue('--chart-zero').trim(),
      muted: css.getPropertyValue('--muted').trim(),
      red: css.getPropertyValue('--battery').trim(),
      orange: css.getPropertyValue('--warning').trim(),
      neutral: css.getPropertyValue('--muted').trim(),
      green: css.getPropertyValue('--charge').trim(),
      blue: css.getPropertyValue('--panel').trim(),
    };
  }

  function bandColor(value, currentColors) {
    if (value < -50) return currentColors.red;
    if (value < -5) return currentColors.orange;
    if (value < 5) return currentColors.neutral;
    if (value <= 50) return currentColors.green;
    return currentColors.blue;
  }

  function virtualNow() {
    const last = sorted[sorted.length - 1];
    const monotonicNow = performance.now();
    if (last && referenceDataTime == null) {
      referenceDataTime = projectLiveNow(last, {
        wallNow: Date.now(), monotonicNow,
      });
      referenceMonotonicTime = monotonicNow;
    }
    return referenceDataTime == null
      ? Date.now()
      : referenceDataTime + monotonicNow - referenceMonotonicTime;
  }

  function axisFor(visible) {
    const finite = visible.map((point) => point.net).filter(Number.isFinite);
    const low = Math.min(-60, ...finite);
    const high = Math.max(60, ...finite);
    return {
      low: Math.min(-60, Math.floor(low / 20) * 20),
      high: Math.max(60, Math.ceil(high / 20) * 20),
    };
  }

  function extendedPoints(start, end) {
    let first = sorted.findIndex((point) => point.timestamp >= start);
    if (first < 0) first = sorted.length;
    let last = sorted.length - 1;
    for (let index = first; index < sorted.length; index += 1) {
      if (sorted[index].timestamp > end) {
        last = index;
        break;
      }
    }
    return sorted.slice(first > 0 ? first - 1 : 0, last + 1);
  }

  function draw() {
    if (!canvas || width <= 0 || height <= 0) return;
    const ratio = window.devicePixelRatio || 1;
    const pixelWidth = Math.max(1, Math.round(width * ratio));
    const pixelHeight = Math.max(1, Math.round(height * ratio));
    if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
    if (canvas.height !== pixelHeight) canvas.height = pixelHeight;

    const ctx = canvas.getContext('2d');
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.font = '11px system-ui, sans-serif';
    const currentColors = colors || (colors = palette());
    const plot = {
      left: PADDING.left, top: PADDING.top,
      width: width - PADDING.left - PADDING.right,
      height: height - PADDING.top - PADDING.bottom,
    };
    const end = virtualNow();
    const start = end - WINDOW_MS;
    const visible = sorted.filter((point) => point.timestamp >= start && point.timestamp <= end);
    const axis = axisFor(visible);
    const y = (value) => plot.top + (axis.high - value) * plot.height / (axis.high - axis.low);
    const x = (timestamp) => plot.left + (timestamp - start) * plot.width / WINDOW_MS;

    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    THRESHOLDS.forEach((threshold) => {
      if (threshold < axis.low || threshold > axis.high) return;
      const py = y(threshold);
      ctx.strokeStyle = threshold === 0 ? currentColors.zero : currentColors.grid;
      ctx.lineWidth = threshold === 0 ? 1.5 : 1;
      ctx.beginPath(); ctx.moveTo(plot.left, py); ctx.lineTo(plot.left + plot.width, py); ctx.stroke();
      if (threshold !== 0) {
        ctx.fillStyle = currentColors.muted;
        ctx.fillText(`${threshold} W`, plot.left - 5, py);
      }
    });

    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    [
      [start, '-30s'], [start + 15_000, '-15s'], [end, 'now'],
    ].forEach(([timestamp, label]) => {
      const px = x(timestamp);
      ctx.strokeStyle = currentColors.grid;
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(px, plot.top); ctx.lineTo(px, plot.top + plot.height); ctx.stroke();
      ctx.fillStyle = currentColors.muted;
      ctx.fillText(label, Math.max(plot.left + 14, Math.min(plot.left + plot.width - 14, px)),
        plot.top + plot.height + 7);
    });

    ctx.textAlign = 'right';
    ctx.textBaseline = 'top';
    ctx.fillStyle = currentColors.muted;
    ctx.fillText('Charging +', plot.left + plot.width - 3, plot.top + 3);
    ctx.textBaseline = 'bottom';
    ctx.fillText('Discharging −', plot.left + plot.width - 3, plot.top + plot.height - 3);

    if (!visible.length) {
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(sorted.length ? 'No readings in the last 30 seconds' : 'Waiting for live readings…',
        width / 2, height / 2);
      return;
    }

    const drawPoints = extendedPoints(start, end);
    ctx.save();
    ctx.beginPath(); ctx.rect(plot.left, plot.top, plot.width, plot.height); ctx.clip();
    let previous = null;
    drawPoints.forEach((point) => {
      if (!Number.isFinite(point.net)) {
        previous = null;
        return;
      }
      if (previous && point.timestamp - previous.timestamp <= GAP_MS) {
        ctx.strokeStyle = bandColor((previous.net + point.net) / 2, currentColors);
        ctx.lineWidth = 3;
        ctx.lineJoin = 'round';
        ctx.beginPath();
        ctx.moveTo(x(previous.timestamp), y(previous.net));
        ctx.lineTo(x(point.timestamp), y(point.net));
        ctx.stroke();
      }
      previous = point;
    });
    ctx.restore();
  }

  function loop() {
    draw();
    rafId = active ? requestAnimationFrame(loop) : null;
  }

  $: sorted, active, draw();
  $: if (active && rafId == null) rafId = requestAnimationFrame(loop);

  onMount(() => {
    const resize = () => {
      const rect = canvas.getBoundingClientRect();
      width = rect.width;
      height = rect.height;
      draw();
    };
    const refreshColors = () => { colors = palette(); draw(); };
    const observer = new ResizeObserver(resize);
    observer.observe(canvas);
    window.addEventListener('viewe-theme-change', refreshColors);
    resize();
    return () => {
      observer.disconnect();
      window.removeEventListener('viewe-theme-change', refreshColors);
      if (rafId != null) cancelAnimationFrame(rafId);
      rafId = null;
    };
  });
</script>

<canvas bind:this={canvas} aria-label="Net battery power over the last 30 seconds"></canvas>

<style>
  canvas {
    display: block;
    width: 100%;
    height: 100%;
    min-height: 12rem;
  }
</style>
