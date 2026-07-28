<script>
  import { onMount } from 'svelte';
  import { projectLiveNow } from './liveTime.js';

  export let points = [];
  export let field = 'voltage';
  export let title = '';
  export let unit = '';
  export let colorVariable = '--accent';
  export let previewPoints = [];
  export let showPreviewLegend = false;
  export let active = true;
  export let emptyMessage = 'Waiting for readings…';
  export let yMin = null;
  export let yMax = null;

  const WINDOW_MS = 30_000;
  const GAP_MS = 3_000;
  const REDRAW_INTERVAL_MS = 500;
  const PADDING = { left: 48, right: 8, top: 10, bottom: 24 };
  let canvas;
  let width = 0;
  let height = 180;
  let resizeObserver;
  let raf;
  let redrawTimer;

  $: if (canvas && points && previewPoints) scheduleDraw();
  $: latestOldValue = points.at(-1)?.[field];
  $: latestNewValue = previewPoints.at(-1)?.preview;

  function formatLatest(value) {
    if (!Number.isFinite(value)) return '—';
    const magnitude = Math.abs(value);
    const digits = magnitude >= 100 ? 0 : magnitude >= 10 ? 1 : 2;
    return `${value.toFixed(digits)} ${unit}`;
  }

  function niceStep(span) {
    const power = 10 ** Math.floor(Math.log10(Math.max(span, 0.001)));
    const normalized = span / power;
    return (normalized <= 1.5 ? 1 : normalized <= 3 ? 2 : normalized <= 7 ? 5 : 10) * power;
  }

  function scheduleDraw() {
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(draw);
  }

  function syncRedrawTimer(isActive, target) {
    clearInterval(redrawTimer);
    redrawTimer = isActive && target
      ? setInterval(scheduleDraw, REDRAW_INTERVAL_MS)
      : null;
    scheduleDraw();
  }

  $: syncRedrawTimer(active, canvas);

  function draw() {
    if (!canvas || !width) return;
    const ratio = devicePixelRatio || 1;
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
    const ctx = canvas.getContext('2d');
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    const css = getComputedStyle(document.documentElement);
    const colors = {
      trace: css.getPropertyValue(colorVariable).trim(),
      grid: css.getPropertyValue('--chart-grid').trim(),
      muted: css.getPropertyValue('--muted').trim(),
      preview: css.getPropertyValue('--warning').trim(),
    };
    ctx.clearRect(0, 0, width, height);
    ctx.font = '11px system-ui, sans-serif';
    const sorted = points.filter((point) => Number.isFinite(point?.timestamp))
      .slice().sort((a, b) => a.timestamp - b.timestamp);
    const newestPoint = sorted.at(-1);
    const newest = newestPoint?.timestamp ?? Date.now();
    const end = active && newestPoint ? projectLiveNow(newestPoint) : newest;
    const start = end - WINDOW_MS;
    const visible = sorted.filter((point) => point.timestamp >= start && point.timestamp <= end);
    const preview = previewPoints.filter((point) => Number.isFinite(point?.timestamp) &&
      point.timestamp >= start && point.timestamp <= end);
    // Include the staged trace in the domain so a meaningful calibration
    // change remains visible instead of being clipped at the plot boundary.
    const values = [
      ...visible.map((point) => point[field]),
      ...preview.map((point) => point.preview),
    ].filter(Number.isFinite);
    const plot = { left: PADDING.left, top: PADDING.top,
      width: width - PADDING.left - PADDING.right,
      height: height - PADDING.top - PADDING.bottom };
    if (!values.length) {
      ctx.fillStyle = colors.muted;
      ctx.textAlign = 'center';
      ctx.fillText(emptyMessage, width / 2, height / 2);
      return;
    }
    let low = Math.min(...values), high = Math.max(...values);
    const fixedDomain = Number.isFinite(yMin) && Number.isFinite(yMax) && yMax > yMin;
    let tickValues;
    if (fixedDomain) {
      low = yMin;
      high = yMax;
      tickValues = Array.from({ length: 4 }, (_, index) =>
        low + (high - low) * index / 3);
    } else {
      const minimumSpan = field === 'voltage' ? 1 : 0.5;
      if (high - low < minimumSpan) {
        const middle = (high + low) / 2;
        low = middle - minimumSpan / 2;
        high = middle + minimumSpan / 2;
      }
      const tick = niceStep((high - low) / 4);
      low = Math.floor(low / tick) * tick;
      high = Math.ceil(high / tick) * tick;
      if (high <= low) high = low + tick;
      tickValues = [];
      for (let value = low; value <= high + tick * 0.01; value += tick) {
        tickValues.push(value);
      }
    }
    const x = (timestamp) => plot.left + plot.width * (timestamp - start) / WINDOW_MS;
    const y = (value) => plot.top + plot.height * (high - value) / (high - low);
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    const tickStep = tickValues.length > 1 ? tickValues[1] - tickValues[0] : high - low;
    const tickDigits = fixedDomain ? 1 : (Math.abs(tickStep) < 1 ? 1 : 0);
    for (const value of tickValues) {
      const py = y(value);
      ctx.strokeStyle = colors.grid;
      ctx.beginPath(); ctx.moveTo(plot.left, py); ctx.lineTo(plot.left + plot.width, py); ctx.stroke();
      ctx.fillStyle = colors.muted;
      ctx.fillText(`${value.toFixed(tickDigits)} ${unit}`, plot.left - 5, py);
    }
    for (let seconds = 0; seconds <= 30; seconds += 10) {
      const px = x(start + seconds * 1000);
      ctx.strokeStyle = colors.grid;
      ctx.beginPath(); ctx.moveTo(px, plot.top); ctx.lineTo(px, plot.top + plot.height); ctx.stroke();
      ctx.fillStyle = colors.muted; ctx.textAlign = 'center'; ctx.textBaseline = 'top';
      ctx.fillText(seconds === 30 ? 'now' : `-${30 - seconds}s`, px, plot.top + plot.height + 7);
    }
    ctx.save();
    ctx.beginPath(); ctx.rect(plot.left, plot.top, plot.width, plot.height); ctx.clip();
    ctx.strokeStyle = colors.trace; ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.beginPath();
    let previous = null;
    visible.forEach((point) => {
      const value = point[field];
      if (!Number.isFinite(value)) { previous = null; return; }
      if (previous == null || point.timestamp - previous > GAP_MS) ctx.moveTo(x(point.timestamp), y(value));
      else ctx.lineTo(x(point.timestamp), y(value));
      previous = point.timestamp;
    });
    ctx.stroke(); ctx.restore();
    if (previewPoints.length) {
      ctx.save();
      ctx.beginPath(); ctx.rect(plot.left, plot.top, plot.width, plot.height); ctx.clip();
      ctx.strokeStyle = colors.preview; ctx.lineWidth = 2; ctx.setLineDash([5, 4]); ctx.beginPath();
      let previousPreview = null;
      preview.forEach((point) => {
        const value = point.preview;
        if (!Number.isFinite(value)) { previousPreview = null; return; }
        if (previousPreview == null || point.timestamp - previousPreview > GAP_MS) ctx.moveTo(x(point.timestamp), y(value));
        else ctx.lineTo(x(point.timestamp), y(value));
        previousPreview = point.timestamp;
      });
      ctx.stroke(); ctx.restore();
    }
  }

  onMount(() => {
    resizeObserver = new ResizeObserver(([entry]) => {
      width = entry.contentRect.width;
      scheduleDraw();
    });
    resizeObserver.observe(canvas);
    addEventListener('viewe-theme-change', scheduleDraw);
    return () => {
      resizeObserver.disconnect();
      removeEventListener('viewe-theme-change', scheduleDraw);
      cancelAnimationFrame(raf);
      clearInterval(redrawTimer);
    };
  });
</script>

<div class="chart-block">
  <div class="chart-heading">
    <h4>{title}</h4>
    {#if showPreviewLegend}
      <div class="chart-legend" aria-label="Calibration preview legend">
        <span><i class="legend-line old" style={`--legend-color: var(${colorVariable})`}></i>Old {formatLatest(latestOldValue)}</span>
        <span><i class="legend-line new"></i>New {formatLatest(latestNewValue)}</span>
      </div>
    {/if}
  </div>
  <canvas bind:this={canvas} aria-label={`${title}, last 30 seconds`}></canvas>
</div>

<style>
  .chart-block { min-width: 0; }
  .chart-heading { display: flex; align-items: center; justify-content: space-between; gap: 0.7rem; margin-bottom: 0.2rem; }
  h4 { margin: 0; color: var(--muted); font-size: 0.85rem; font-weight: 500; }
  .chart-legend { display: flex; align-items: center; gap: 0.75rem; color: var(--muted); font-size: 0.75rem; }
  .chart-legend span { display: inline-flex; align-items: center; gap: 0.3rem; }
  .legend-line { display: inline-block; width: 1.25rem; border-top: 2px solid; }
  .legend-line.old { border-color: var(--legend-color); }
  .legend-line.new { border-color: var(--warning); border-top-style: dashed; }
  canvas { display: block; width: 100%; height: 180px; }
</style>
