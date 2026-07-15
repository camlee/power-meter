<script>
  import { onMount } from 'svelte';

  export let buckets = [];

  // ---------------------------------------------------------------------
  // Constants
  // ---------------------------------------------------------------------

  const MOBILE_BREAKPOINT_PX = 460;
  const BAR_INSET_RATIO = 0.2; // fraction of bucket width left as gap between bars
  const MAX_TIME_LABELS = 6;
  const CHART_FONT = '11px system-ui, sans-serif';

  const timeLabelFormatter = new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' });

  // ---------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------

  let canvas;

  // ---------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------

  function palette() {
    const css = getComputedStyle(document.documentElement);
    return {
      grid: css.getPropertyValue('--chart-grid').trim(),
      zero: css.getPropertyValue('--chart-zero').trim(),
      muted: css.getPropertyValue('--muted').trim(),
      text: css.getPropertyValue('--text').trim(),
      charge: css.getPropertyValue('--charge').trim(),
      panel: css.getPropertyValue('--panel').trim(),
      surplus: css.getPropertyValue('--surplus').trim(),
      battery: css.getPropertyValue('--battery').trim(),
      load: css.getPropertyValue('--load').trim(),
    };
  }

  // Nice round step for the y-axis (watts), e.g. 1/2/5/10/20/50/100...
  function niceStep(span) {
    const magnitude = 10 ** Math.floor(Math.log10(Math.max(span, 1)));
    const normalized = span / magnitude;
    const base = normalized <= 1.5 ? 1 : normalized <= 3 ? 2 : normalized <= 7 ? 5 : 10;
    return base * magnitude;
  }

  function formatValueLabel(value) {
    return Math.abs(value) >= 100 ? `${Math.round(value)}` : `${Math.round(value * 10) / 10}`;
  }

  function computePadding(mobile) {
    return { left: mobile ? 38 : 47, right: 10, top: 16, bottom: 29 };
  }

  // ---------------------------------------------------------------------
  // Axis computation
  // ---------------------------------------------------------------------

  // Positive stack = charging + panelIn + panelSurplus (above zero).
  // Negative stack = batteryUsage + panelUsage (below zero, drawn downward).
  function computeYAxis(coveredBuckets) {
    let low = 0;
    let high = 0;
    const sumFinite = (...values) => values.reduce(
      (total, value) => total + (Number.isFinite(value) ? value : 0), 0,
    );
    coveredBuckets.forEach((bucket) => {
      high = Math.max(high, sumFinite(bucket.charging, bucket.panelIn, bucket.panelSurplus));
      low = Math.min(low, -sumFinite(bucket.batteryUsage, bucket.panelUsage));
    });

    const step = niceStep((high - low || 1) / 4);
    return {
      low: Math.min(-step, Math.floor(low / step) * step),
      high: Math.max(step, Math.ceil(high / step) * step),
      step,
    };
  }

  // ---------------------------------------------------------------------
  // Drawing
  // ---------------------------------------------------------------------

  function drawYAxis(ctx, plot, axis, colors) {
    const valueToY = (value) => plot.top + ((axis.high - value) * plot.height) / (axis.high - axis.low);

    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';

    for (let value = axis.low; value <= axis.high + axis.step * 0.01; value += axis.step) {
      const y = valueToY(value);
      ctx.strokeStyle = value === 0 ? colors.zero : colors.grid;
      ctx.lineWidth = value === 0 ? 1.25 : 1;
      ctx.beginPath();
      ctx.moveTo(plot.left, y);
      ctx.lineTo(plot.left + plot.width, y);
      ctx.stroke();

      ctx.fillStyle = colors.muted;
      ctx.fillText(formatValueLabel(value), plot.left - 6, y);
    }

    return valueToY;
  }

  // Draws one bucket's positive or negative stack of segments as a single
  // bar, starting from zero and growing outward in `direction`.
  function drawStack(ctx, x, barWidth, values, direction, valueToY) {
    let cursor = 0;
    values.forEach(([value, color]) => {
      if (!Number.isFinite(value) || value <= 0) return;
      const from = valueToY(cursor * direction);
      const to = valueToY((cursor + value) * direction);
      ctx.fillStyle = color;
      ctx.fillRect(x, Math.min(from, to), barWidth, Math.max(1, Math.abs(to - from)));
      cursor += value;
    });
  }

  function drawBars(ctx, plot, coveredBuckets, bucketWidth, barWidth, valueToY, colors) {
    coveredBuckets.forEach(({ bucket, index }) => {
      const x = plot.left + index * bucketWidth + (bucketWidth - barWidth) / 2;
      drawStack(
        ctx, x, barWidth,
        [[bucket.charging, colors.charge], [bucket.panelIn, colors.panel], [bucket.panelSurplus, colors.surplus]],
        1, valueToY,
      );
      drawStack(
        ctx, x, barWidth,
        [[bucket.batteryUsage, colors.battery], [bucket.panelUsage, colors.load]],
        -1, valueToY,
      );
    });
  }

  function drawTimeLabels(ctx, plot, buckets, bucketWidth, colors) {
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    ctx.fillStyle = colors.muted;

    const shown = Math.min(MAX_TIME_LABELS, buckets.length);
    for (let i = 0; i < shown; i += 1) {
      const bucketIndex = Math.round((i * (buckets.length - 1)) / Math.max(1, shown - 1));
      const stamp = buckets[bucketIndex]?.unixMs;
      if (!Number.isFinite(stamp) || stamp <= 0) continue;

      const text = timeLabelFormatter.format(new Date(stamp));
      const x = plot.left + (bucketIndex + 0.5) * bucketWidth;
      ctx.fillText(text, x, plot.top + plot.height + 8);
    }
  }

  function drawAxisTitle(ctx, plot, colors) {
    ctx.save();
    ctx.translate(11, plot.top + plot.height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = colors.muted;
    ctx.fillText('Average power [W]', 0, 0);
    ctx.restore();
  }

  function drawMessage(ctx, width, height, colors, text) {
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = colors.muted;
    ctx.fillText(text, width / 2, height / 2);
  }

  function draw() {
    if (!canvas) return;

    const rect = canvas.getBoundingClientRect();
    const ratio = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(rect.width * ratio));
    canvas.height = Math.max(1, Math.round(rect.height * ratio));

    const ctx = canvas.getContext('2d');
    ctx.scale(ratio, ratio);

    const width = rect.width;
    const height = rect.height;
    const colors = palette();

    ctx.clearRect(0, 0, width, height);
    ctx.font = CHART_FONT;

    if (!buckets.length) {
      drawMessage(ctx, width, height, colors, 'No history data yet.');
      return;
    }

    const pad = computePadding(width < MOBILE_BREAKPOINT_PX);
    const plot = {
      left: pad.left,
      top: pad.top,
      width: Math.max(1, width - pad.left - pad.right),
      height: Math.max(1, height - pad.top - pad.bottom),
    };

    // Only buckets with actual coverage contribute to the axis range and
    // get bars drawn; uncovered intervals are skipped rather than shown
    // as zero-height bars (which would misleadingly read as "0 W").
    const coveredBuckets = buckets
      .map((bucket, index) => ({ bucket, index }))
      .filter(({ bucket }) => bucket.componentCoverageMs?.some((coverage) => coverage));

    const axis = computeYAxis(coveredBuckets.map(({ bucket }) => bucket));
    const valueToY = drawYAxis(ctx, plot, axis, colors);

    const bucketWidth = plot.width / buckets.length;
    const barWidth = Math.max(1, bucketWidth - Math.max(1, bucketWidth * BAR_INSET_RATIO));
    drawBars(ctx, plot, coveredBuckets, bucketWidth, barWidth, valueToY, colors);

    drawTimeLabels(ctx, plot, buckets, bucketWidth, colors);
    drawAxisTitle(ctx, plot, colors);

    if (!coveredBuckets.length) {
      drawMessage(ctx, width, height, colors, 'No data available for this period.');
    }
  }

  // ---------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------

  $: buckets, draw();

  onMount(() => {
    const observer = new ResizeObserver(draw);
    observer.observe(canvas);
    window.addEventListener('viewe-theme-change', draw);
    return () => {
      observer.disconnect();
      window.removeEventListener('viewe-theme-change', draw);
    };
  });
</script>

<canvas bind:this={canvas} aria-label="Stacked history power graph with time and watt axes"></canvas>

<style>
  canvas {
    width: 100%;
    height: clamp(20rem, calc(100dvh - 10rem), 42rem);
    display: block;
  }
</style>
