function validMultiplier(value) {
  return value === -1 ? -1 : 1;
}

export function adcInputFromEngineering(displayed, calibration, multiplier = 1) {
  if (!Number.isFinite(displayed) || !Number.isFinite(calibration?.gain) ||
      calibration.gain <= 0 || !Number.isFinite(calibration?.offset_input_v)) {
    return Number.NaN;
  }
  return displayed / validMultiplier(multiplier) / calibration.gain +
    calibration.offset_input_v;
}

export function calibratedPreview(input, gain, offset, multiplier = 1) {
  if (!Number.isFinite(input) || !Number.isFinite(gain) ||
      !Number.isFinite(offset)) return Number.NaN;
  return validMultiplier(multiplier) * (input - offset) * gain;
}

export function calculatedGain(reference, input, offset, multiplier = 1) {
  const denominator = validMultiplier(multiplier) * (input - offset);
  if (!Number.isFinite(reference) || reference <= 0 ||
      !Number.isFinite(denominator) || Math.abs(denominator) < 0.005) {
    return Number.NaN;
  }
  const gain = reference / denominator;
  return Number.isFinite(gain) && gain > 0 ? gain : Number.NaN;
}
