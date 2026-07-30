<script>
  import { onMount } from 'svelte';
  import { usageBreakdown } from './powerFlow.js';

  export let buckets = [];
  export let timelineBasis = 'wall-clock';
  export let startTimeMs = 0;
  export let endTimeMs = 0;
  export let tickMinutes = 0;
  export let pwmUiEnabled = false;
  export let showBalance = false;

  // ---------------------------------------------------------------------
  // Constants
  // ---------------------------------------------------------------------

  const MOBILE_BREAKPOINT_PX = 460;
  const BAR_INSET_RATIO = 0.2; // fraction of bucket width left as gap between bars
  const MIN_TIME_LABEL_SPACING_PX = 38;
  const CHART_FONT = '11px system-ui, sans-serif';
  const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

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
      balance: css.getPropertyValue('--muted').trim(),
    };
  }

  // Nice round step for the y-axis (watts), e.g. 1/2/5/10/20/50/100...
  function niceStep(span) {
    const magnitude = 10 ** Math.floor(Math.log10(Math.max(span, Number.EPSILON)));
    const normalized = span / magnitude;
    const base = normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10;
    return base * magnitude;
  }

  function formatValueLabel(value) {
    return `${Math.round(value)} W`;
  }

  function computePadding(mobile) {
    return { left: mobile ? 43 : 48, right: 10, top: 16, bottom: 29 };
  }

  function automaticTickMinutes(durationMinutes) {
    const choices = [15, 30, 60, 120, 240, 360, 720, 1440, 2880, 4320, 10080];
    const target = Math.ceil(durationMinutes / 7);
    return choices.find((choice) => choice >= target) ?? choices[choices.length - 1];
  }

  // Use weekdays for multi-day views; shorter views have room for a complete
  // local time such as "3:00 PM" or "3:30 PM".
  function formatTimeLabel(unixMs, durationMinutes, intervalMinutes) {
    const date = new Date(unixMs);
    if (durationMinutes > 2 * 1440 && intervalMinutes >= 1440) return WEEKDAYS[date.getDay()];
    const hour24 = date.getHours();
    const hour = hour24 % 12 || 12;
    const minutes = String(date.getMinutes()).padStart(2, '0');
    return `${hour}:${minutes} ${hour24 < 12 ? 'AM' : 'PM'}`;
  }

  function formatRelativeLabel(timeMs) {
    const minutes = Math.max(0, Math.round((endTimeMs - timeMs) / 60_000));
    if (!minutes) return 'now';
    if (minutes < 60) return `${minutes}m ago`;
    if (minutes < 1440) return `${Math.round(minutes / 60)}h ago`;
    return `${Math.round(minutes / 1440)}d ago`;
  }

  // ---------------------------------------------------------------------
  // Axis computation
  // ---------------------------------------------------------------------

  function flowForBucket(bucket) {
    const batteryMeasured = Number.isFinite(bucket.aux);
    const battery = batteryMeasured
      ? bucket.aux
      : (Number.isFinite(bucket.charging) && Number.isFinite(bucket.batteryUsage)
        ? bucket.charging - bucket.batteryUsage
        : Number.NaN);
    return usageBreakdown(
      bucket.in, bucket.out, battery, batteryMeasured, showBalance,
    );
  }

  function computeYAxis(coveredBuckets) {
    let low = 0;
    let high = 0;
    const sumFinite = (...values) => values.reduce(
      (total, value) => total + (Number.isFinite(value) ? value : 0), 0,
    );
    coveredBuckets.forEach((bucket) => {
      if (pwmUiEnabled) {
        high = Math.max(high, sumFinite(bucket.charging, bucket.panelIn, bucket.panelSurplus));
        low = Math.min(low, -sumFinite(bucket.batteryUsage, bucket.panelUsage));
      } else {
        const flow = flowForBucket(bucket);
        const endpoints = [
          flow.solarSegment, flow.loadSegment, flow.chargeSegment,
          flow.dischargeSegment, flow.balanceSegment,
        ].flatMap((segment) => [segment.from, segment.to])
          .filter(Number.isFinite);
        high = Math.max(high, flow.solarTotal, ...endpoints);
        low = Math.min(low, -flow.loadTotal, ...endpoints);
      }
    });

    const step = niceStep(Math.max(high - low, 1) / 6);
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

  function drawRange(ctx, x, barWidth, range, color, valueToY) {
    if (!Number.isFinite(range?.from) || !Number.isFinite(range?.to) ||
        Math.abs(range.to - range.from) <= 0.0001) return;
    const from = valueToY(range.from);
    const to = valueToY(range.to);
    ctx.fillStyle = color;
    ctx.fillRect(x, Math.min(from, to), barWidth, Math.max(1, Math.abs(to - from)));
  }

  function drawBars(ctx, plot, coveredBuckets, bucketWidth, barWidth, valueToY, colors) {
    coveredBuckets.forEach(({ bucket, index }) => {
      const x = plot.left + index * bucketWidth + (bucketWidth - barWidth) / 2;
      if (pwmUiEnabled) {
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
      } else {
        const flow = flowForBucket(bucket);
        drawRange(ctx, x, barWidth, flow.chargeSegment, colors.charge, valueToY);
        drawRange(ctx, x, barWidth, flow.solarSegment, colors.panel, valueToY);
        drawRange(ctx, x, barWidth, flow.loadSegment, colors.load, valueToY);
        drawRange(ctx, x, barWidth, flow.dischargeSegment, colors.battery, valueToY);
        if (showBalance) {
          drawRange(ctx, x, barWidth, flow.balanceSegment, colors.balance, valueToY);
        }
      }
    });
  }

  function drawTimeLabels(ctx, plot, colors) {
    const durationMinutes = Math.max(1, Math.round((endTimeMs - startTimeMs) / 60_000));
    const intervalMinutes = tickMinutes || automaticTickMinutes(durationMinutes);
    const intervalMs = intervalMinutes * 60_000;
    if (!Number.isFinite(startTimeMs) || !Number.isFinite(endTimeMs) ||
        endTimeMs <= startTimeMs || intervalMs <= 0) return;

    let ticks = [];
    if (timelineBasis === 'relative') {
      for (let tick = endTimeMs; tick >= startTimeMs; tick -= intervalMs) ticks.push(tick);
      ticks.reverse();
    } else {
      // Align wall-clock ticks to local boundaries, as the fixed-offset device does.
      const localOffsetMs = -new Date(startTimeMs).getTimezoneOffset() * 60_000;
      const firstTick = Math.ceil((startTimeMs + localOffsetMs) / intervalMs) * intervalMs - localOffsetMs;
      for (let tick = firstTick; tick <= endTimeMs; tick += intervalMs) ticks.push(tick);
    }
    const tickPixels = plot.width * intervalMs / Math.max(1, endTimeMs - startTimeMs);
    const labelEvery = Math.max(1, Math.ceil(MIN_TIME_LABEL_SPACING_PX / Math.max(1, tickPixels)));
    let previousLabelRight = -Infinity;

    ctx.textBaseline = 'top';
    ctx.fillStyle = colors.muted;
    for (let index = 0; index < ticks.length; index += 1) {
      if (index % labelEvery) continue;
      const tick = ticks[index];
      const x = plot.left + plot.width * (tick - startTimeMs) / (endTimeMs - startTimeMs);

      ctx.strokeStyle = colors.grid;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, plot.top);
      ctx.lineTo(x, plot.top + plot.height);
      ctx.stroke();

      const text = timelineBasis === 'relative'
        ? formatRelativeLabel(tick)
        : formatTimeLabel(tick, durationMinutes, intervalMinutes);
      const textWidth = ctx.measureText(text).width;
      const labelX = Math.max(1, Math.min(x - textWidth / 2, plot.left + plot.width - textWidth));
      if (labelX < previousLabelRight + 4) continue;
      ctx.textAlign = 'left';
      ctx.fillText(text, labelX, plot.top + plot.height + 8);
      previousLabelRight = labelX + textWidth;
    }
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
      .filter(({ bucket }) => (pwmUiEnabled ? bucket.componentCoverageMs : bucket.channelCoverageMs)
        ?.some((coverage) => coverage));

    const axis = computeYAxis(coveredBuckets.map(({ bucket }) => bucket));
    const valueToY = drawYAxis(ctx, plot, axis, colors);

    const bucketWidth = plot.width / buckets.length;
    const barWidth = Math.max(1, bucketWidth - Math.max(1, bucketWidth * BAR_INSET_RATIO));
    drawTimeLabels(ctx, plot, colors);
    drawBars(ctx, plot, coveredBuckets, bucketWidth, barWidth, valueToY, colors);

    if (!coveredBuckets.length) {
      drawMessage(ctx, width, height, colors, 'No data available for this period.');
    }
  }

  // ---------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------

  $: buckets, timelineBasis, startTimeMs, endTimeMs, tickMinutes,
    pwmUiEnabled, showBalance, draw();

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

<canvas bind:this={canvas}
  aria-label={`Stacked history power graph${showBalance
    ? ' with Balance segments' : ''}, time, and watt axes`}></canvas>

<style>
  canvas {
    flex: 1;
    width: 100%;
    height: auto;
    min-height: 0;
    display: block;
  }
</style>
