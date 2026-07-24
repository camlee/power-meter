<script>
  import { onMount } from 'svelte';

  export let capture = null;

  const HEIGHT = 310;
  const PADDING = { left: 52, right: 10, top: 12, bottom: 25 };
  let canvas;
  let width = 0;
  let resizeObserver;
  let raf;

  $: if (canvas && capture) scheduleDraw();

  function scheduleDraw() {
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(draw);
  }

  function niceStep(span) {
    const power = 10 ** Math.floor(Math.log10(Math.max(span, 0.001)));
    const normalized = span / power;
    return (normalized <= 1.5 ? 1 : normalized <= 3 ? 2 : normalized <= 7 ? 5 : 10) * power;
  }

  function drawPanel(ctx, panel, field, unit, traceColor, averageColor, colors) {
    const values = capture.points.map((point) => point[field]).filter(Number.isFinite);
    const averages = capture.windows.map((window) => window[field]).filter(Number.isFinite);
    const domainValues = [...values, ...averages];
    if (!domainValues.length) return;
    let low = Math.min(...domainValues), high = Math.max(...domainValues);
    const minimumSpan = field === 'voltage' ? 0.5 : 0.25;
    if (high - low < minimumSpan) {
      const middle = (high + low) / 2;
      low = middle - minimumSpan / 2;
      high = middle + minimumSpan / 2;
    }
    const tick = niceStep((high - low) / 3);
    low = Math.floor(low / tick) * tick;
    high = Math.ceil(high / tick) * tick;
    if (high <= low) high = low + tick;

    const duration = Math.max(1, capture.durationUs);
    const x = (elapsedUs) => panel.left + panel.width * elapsedUs / duration;
    const y = (value) => panel.top + panel.height * (high - value) / (high - low);

    ctx.font = '11px system-ui, sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let value = low; value <= high + tick * 0.01; value += tick) {
      const py = y(value);
      ctx.strokeStyle = colors.grid;
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(panel.left, py); ctx.lineTo(panel.left + panel.width, py); ctx.stroke();
      ctx.fillStyle = colors.muted;
      ctx.fillText(`${value.toFixed(Math.abs(tick) < 1 ? 1 : 0)} ${unit}`, panel.left - 5, py);
    }

    capture.windows.forEach((window) => {
      const px = x(window.startUs);
      ctx.strokeStyle = colors.grid;
      ctx.beginPath(); ctx.moveTo(px, panel.top); ctx.lineTo(px, panel.top + panel.height); ctx.stroke();
    });
    ctx.strokeStyle = colors.grid;
    ctx.beginPath(); ctx.moveTo(x(duration), panel.top); ctx.lineTo(x(duration), panel.top + panel.height); ctx.stroke();

    ctx.save();
    ctx.beginPath(); ctx.rect(panel.left, panel.top, panel.width, panel.height); ctx.clip();
    ctx.strokeStyle = traceColor;
    ctx.lineWidth = 1.25;
    ctx.lineJoin = 'round';
    ctx.beginPath();
    let started = false;
    capture.points.forEach((point) => {
      if (!Number.isFinite(point[field])) return;
      if (!started) {
        ctx.moveTo(x(point.elapsedUs), y(point[field]));
        started = true;
      } else {
        ctx.lineTo(x(point.elapsedUs), y(point[field]));
      }
    });
    ctx.stroke();

    ctx.lineWidth = 2.5;
    capture.windows.forEach((window) => {
      if (!Number.isFinite(window[field])) return;
      ctx.strokeStyle = window.eligible
        ? averageColor
        : window.state === 'out_of_range' ? colors.warning : colors.rejected;
      ctx.setLineDash(window.eligible ? [] : [5, 3]);
      ctx.beginPath();
      ctx.moveTo(x(window.startUs), y(window[field]));
      ctx.lineTo(x(window.endUs), y(window[field]));
      ctx.stroke();
    });
    ctx.setLineDash([]);
    ctx.restore();
  }

  function draw() {
    if (!canvas || !width || !capture?.points?.length) return;
    const ratio = devicePixelRatio || 1;
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(HEIGHT * ratio);
    const ctx = canvas.getContext('2d');
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, HEIGHT);
    const css = getComputedStyle(document.documentElement);
    const colors = {
      grid: css.getPropertyValue('--chart-grid').trim(),
      muted: css.getPropertyValue('--muted').trim(),
      voltage: css.getPropertyValue('--panel').trim(),
      current: css.getPropertyValue('--warning').trim(),
      average: css.getPropertyValue('--charge').trim(),
      warning: css.getPropertyValue('--warning').trim(),
      rejected: css.getPropertyValue('--battery').trim(),
    };
    const plotHeight = (HEIGHT - PADDING.top - PADDING.bottom - 28) / 2;
    const voltagePanel = {
      left: PADDING.left, top: PADDING.top,
      width: width - PADDING.left - PADDING.right, height: plotHeight,
    };
    const currentPanel = {
      left: PADDING.left, top: PADDING.top + plotHeight + 28,
      width: voltagePanel.width, height: plotHeight,
    };
    drawPanel(ctx, voltagePanel, 'voltage', 'V', colors.voltage, colors.average, colors);
    drawPanel(ctx, currentPanel, 'current', 'A', colors.current, colors.average, colors);

    ctx.fillStyle = colors.muted;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    const durationSeconds = capture.durationUs / 1_000_000;
    capture.windows.forEach((window, index) => {
      const middleUs = (window.startUs + window.endUs) / 2;
      const px = currentPanel.left + currentPanel.width * middleUs / capture.durationUs;
      ctx.fillText(`Window ${index + 1}`, px, currentPanel.top + currentPanel.height + 7);
    });
    ctx.textAlign = 'right';
    ctx.fillText(`${durationSeconds.toFixed(2)} s`, currentPanel.left + currentPanel.width,
      currentPanel.top + currentPanel.height + 7);
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
    };
  });
</script>

<div class="capture-chart">
  <div class="legend" aria-label="ADC capture legend">
    <span><i class="sample"></i>Raw observations</span>
    <span><i class="average"></i>500 ms production value</span>
    <span><i class="rejected"></i>Rejected production window</span>
  </div>
  <canvas bind:this={canvas} aria-label="Captured input voltage and current"></canvas>
</div>

<style>
  .capture-chart { min-width: 0; }
  .legend {
    display: flex;
    flex-wrap: wrap;
    justify-content: flex-end;
    gap: 0.75rem;
    color: var(--muted);
    font-size: 0.75rem;
  }
  .legend span { display: inline-flex; align-items: center; gap: 0.3rem; }
  .legend i { display: inline-block; width: 1.25rem; border-top: 2px solid var(--panel); }
  .legend .average { border-color: var(--charge); border-width: 3px; }
  .legend .rejected {
    border-color: var(--battery);
    border-top-style: dashed;
    border-width: 3px;
  }
  canvas { display: block; width: 100%; height: 310px; }
</style>
