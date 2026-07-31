import test from 'node:test';
import assert from 'node:assert/strict';

import {
  adcInputFromEngineering,
  calibrationValidationMessage,
  calibratedPreview,
  calculatedGain,
  gainToMillivoltsPerUnit,
  millivoltsPerUnitToGain,
  validateCalibration,
} from '../src/lib/calibrationMath.js';

const calibration = { gain: 40, offset_input_v: 1.667 };

test('web gain fields use the same millivolts-per-unit representation as LVGL', () => {
  assert.equal(gainToMillivoltsPerUnit(40), 25);
  assert.equal(millivoltsPerUnitToGain(25), 40);
  assert.equal(Number.isNaN(gainToMillivoltsPerUnit(0)), true);
  assert.equal(Number.isNaN(millivoltsPerUnitToGain(0)), true);
});

test('normal current calibration retains the existing conversion', () => {
  assert.equal(calibratedPreview(1.917, 40, 1.667), 10);
  assert.equal(adcInputFromEngineering(10, calibration), 1.917);
  assert.equal(calculatedGain(10, 1.917, 1.667), 40);
});

test('reversed current calibration keeps effective charging current positive', () => {
  assert.equal(calibratedPreview(1.417, 40, 1.667, -1), 10);
  assert.equal(adcInputFromEngineering(10, calibration, -1), 1.417);
  assert.equal(calculatedGain(10, 1.417, 1.667, -1), 40);
});

test('a reference inconsistent with the configured direction is rejected', () => {
  assert.equal(
    Number.isNaN(calculatedGain(10, 1.917, 1.667, -1)),
    true,
  );
});

test('current gain calculation accepts a matching negative reference', () => {
  assert.equal(
    calculatedGain(-10, 1.417, 1.667, 1, true),
    40,
  );
  assert.equal(
    Number.isNaN(calculatedGain(-10, 1.417, 1.667)),
    true,
  );
});

test('calibration validation uses the complete ADC range', () => {
  const cases = [
    ['voltage', 21.3, 0.01, ''],
    ['current', 33, 1.1613, ''],
    ['voltage', 250 / 3.3, 0, ''],
    ['voltage', 250.01 / 3.3, 0, 'output_above_sanity'],
    ['current', 150 / 3.3, 3.3, ''],
    ['current', 150.01 / 3.3, 3.3, 'output_below_sanity'],
    ['voltage', 100, 0, 'output_above_sanity'],
    ['current', 100, 1.65, 'output_above_sanity'],
    ['current', 100, 3.3, 'output_below_sanity'],
    ['voltage', 21.3, -0.01, 'offset_below_adc'],
    ['voltage', 21.3, 3.31, 'offset_above_adc'],
    ['voltage', 0, 0, 'gain_not_positive'],
  ];
  for (const [measurement, gain, offset, issue] of cases) {
    const result = validateCalibration(measurement, gain, offset);
    assert.equal(result.issue, issue);
    assert.equal(result.valid, issue === '');
  }
});

test('gain validation is expressed in engineering output units', () => {
  const voltage = validateCalibration('voltage', 100, 0);
  assert.equal(voltage.impliedOutput, 330);
  assert.equal(voltage.limit, 250);
  assert.equal(
    calibrationValidationMessage(voltage, 'voltage', 'Gain'),
    'Gain implied 330.00 V > 250 V sanity limit.',
  );

  const current = validateCalibration('current', 100, 3.3);
  assert.ok(Math.abs(current.impliedOutput + 330) < 1e-9);
  assert.equal(
    calibrationValidationMessage(current, 'current', 'Gain'),
    'Gain implied -330.00 A < -150 A sanity limit.',
  );
});
