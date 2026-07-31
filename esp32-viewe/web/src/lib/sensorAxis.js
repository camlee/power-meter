export function niceSensorStep(target, minimum = 1) {
  const bounded = Math.max(Number.isFinite(target) ? target : minimum, minimum);
  const magnitude = 10 ** Math.floor(Math.log10(bounded));
  const normalized = bounded / magnitude;
  const base = normalized <= 1 ? 1
    : normalized <= 2 ? 2
      : normalized <= 5 ? 5 : 10;
  return base * magnitude;
}

function roundedAxis(low, high, targetIntervals = 4) {
  if (high - low < 1) {
    const middle = (high + low) / 2;
    low = middle - 0.5;
    high = middle + 0.5;
  }
  const step = niceSensorStep((high - low) / targetIntervals);
  let axisLow = Math.floor(low / step) * step;
  let axisHigh = Math.ceil(high / step) * step;
  if (axisHigh <= axisLow) axisHigh = axisLow + step;
  return { low: axisLow, high: axisHigh, step };
}

export function sensorAxis(field, values) {
  const finite = values.filter(Number.isFinite);
  const observedLow = finite.length ? Math.min(...finite) : 0;
  const observedHigh = finite.length ? Math.max(...finite) : 1;

  if (field === 'voltage') {
    return roundedAxis(
      observedLow < 12 ? observedLow - 0.2 : 12,
      observedHigh > 14 ? observedHigh + 0.2 : 14,
    );
  }
  if (field === 'current') {
    return roundedAxis(
      observedLow < 0 ? observedLow - 0.3 : 0,
      observedHigh > 5 ? observedHigh + 0.3 : 5,
    );
  }
  return roundedAxis(observedLow, observedHigh);
}

export function calibrationAxis(values, measurement) {
  const finite = values.filter(Number.isFinite);
  if (!finite.length) return { low: -1, high: 1, step: 1 };
  const observedLow = Math.min(...finite);
  const observedHigh = Math.max(...finite);
  const margin = Math.max(
    measurement === 'voltage' ? 0.5 : 1,
    (observedHigh - observedLow) * 0.2,
  );
  return roundedAxis(
    Math.min(0, observedLow - margin),
    Math.max(0, observedHigh + margin),
    5,
  );
}

export function fixedSensorAxis(low, high) {
  const step = niceSensorStep((high - low) / 4);
  const firstTick = Math.ceil(low / step) * step;
  const ticks = [];
  for (let value = firstTick; value <= high + step * 0.001; value += step) {
    ticks.push(Math.abs(value) < step * 0.001 ? 0 : value);
  }
  return { low, high, step, ticks };
}
