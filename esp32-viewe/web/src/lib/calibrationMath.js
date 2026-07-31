function validMultiplier(value) {
  return value === -1 ? -1 : 1;
}

export const ADC_MIN_INPUT_V = 0;
export const ADC_MAX_INPUT_V = 3.3;
export const MAX_STORED_GAIN_PER_INPUT_V = 10000;
export const MAX_VOLTAGE_V = 250;
export const MAX_CURRENT_A = 150;

export function gainToMillivoltsPerUnit(gain) {
  return Number.isFinite(gain) && gain > 0 ? 1000 / gain : Number.NaN;
}

export function millivoltsPerUnitToGain(value) {
  return Number.isFinite(value) && value > 0 ? 1000 / value : Number.NaN;
}

export function validateCalibration(measurement, gain, offset) {
  const result = {
    valid: false,
    issue: '',
    impliedOutput: Number.NaN,
    limit: Number.NaN,
  };
  if (!Number.isFinite(gain)) {
    result.issue = 'gain_not_finite';
    return result;
  }
  if (gain <= 0) {
    result.issue = 'gain_not_positive';
    return result;
  }
  if (!Number.isFinite(offset)) {
    result.issue = 'offset_not_finite';
    return result;
  }
  if (offset < ADC_MIN_INPUT_V) {
    result.issue = 'offset_below_adc';
    result.limit = ADC_MIN_INPUT_V;
    return result;
  }
  if (offset > ADC_MAX_INPUT_V) {
    result.issue = 'offset_above_adc';
    result.limit = ADC_MAX_INPUT_V;
    return result;
  }
  const atMinimumInput = (ADC_MIN_INPUT_V - offset) * gain;
  const atMaximumInput = (ADC_MAX_INPUT_V - offset) * gain;
  const minimum = Math.min(atMinimumInput, atMaximumInput);
  const maximum = Math.max(atMinimumInput, atMaximumInput);
  if (!Number.isFinite(minimum) || !Number.isFinite(maximum)) {
    result.issue = 'output_not_finite';
    return result;
  }
  const magnitudeLimit =
    measurement === 'voltage' ? MAX_VOLTAGE_V : MAX_CURRENT_A;
  if (maximum > magnitudeLimit) {
    result.issue = 'output_above_sanity';
    result.impliedOutput = maximum;
    result.limit = magnitudeLimit;
    return result;
  }
  if (minimum < -magnitudeLimit) {
    result.issue = 'output_below_sanity';
    result.impliedOutput = minimum;
    result.limit = -magnitudeLimit;
    return result;
  }
  if (gain > MAX_STORED_GAIN_PER_INPUT_V) {
    result.issue = 'gain_storage_limit';
    return result;
  }
  result.valid = true;
  return result;
}

export function calibrationValidationMessage(
  validation, measurement, subject = 'Calibration',
) {
  const unit = measurement === 'voltage' ? 'V' : 'A';
  switch (validation?.issue) {
    case 'gain_not_finite':
      return 'Gain must be a finite number.';
    case 'gain_not_positive':
      return 'Gain must be greater than zero.';
    case 'gain_storage_limit':
    case 'output_not_finite':
      return `${subject} implied a non-finite value.`;
    case 'offset_not_finite':
      return 'Offset must be a finite number.';
    case 'offset_below_adc':
      return `Offset is below the ${validation.limit} V ADC limit.`;
    case 'offset_above_adc':
      return `Offset is above the ${validation.limit} V ADC limit.`;
    case 'output_above_sanity':
      return `${subject} implied ${validation.impliedOutput.toPrecision(5)} ${unit} > ${validation.limit} ${unit} sanity limit.`;
    case 'output_below_sanity':
      return `${subject} implied ${validation.impliedOutput.toPrecision(5)} ${unit} < ${validation.limit} ${unit} sanity limit.`;
    default:
      return '';
  }
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

export function calculatedGain(
  reference, input, offset, multiplier = 1, allowSignedReference = false,
) {
  const denominator = validMultiplier(multiplier) * (input - offset);
  if (!Number.isFinite(reference) ||
      (allowSignedReference ? reference === 0 : reference <= 0) ||
      !Number.isFinite(denominator) || Math.abs(denominator) < 0.005) {
    return Number.NaN;
  }
  const gain = reference / denominator;
  return Number.isFinite(gain) && gain > 0 ? gain : Number.NaN;
}
