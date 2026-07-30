export function niceHomeStep(target) {
  const bounded = Math.max(Number.isFinite(target) ? target : 1, 1);
  const magnitude = 10 ** Math.floor(Math.log10(bounded));
  const normalized = bounded / magnitude;
  const base = normalized <= 1 ? 1
    : normalized <= 2 ? 2
      : normalized <= 5 ? 5 : 10;
  return base * magnitude;
}

export function homeAxis(observedMinimum, observedMaximum) {
  if (!Number.isFinite(observedMinimum) ||
      !Number.isFinite(observedMaximum)) {
    return { low: -1, high: 1, step: 1 };
  }
  const low = Math.min(observedMinimum, 0);
  const high = Math.max(observedMaximum, 0);
  const step = niceHomeStep((high - low) / 5);
  let axisLow = Math.floor(low / step) * step;
  let axisHigh = Math.ceil(high / step) * step;
  if (axisHigh <= axisLow) {
    axisLow = -step;
    axisHigh = step;
  }
  return { low: axisLow, high: axisHigh, step };
}
