import test from 'node:test';
import assert from 'node:assert/strict';

import {
  adcInputFromEngineering,
  calibratedPreview,
  calculatedGain,
} from '../src/lib/calibrationMath.js';

const calibration = { gain: 40, offset_input_v: 1.667 };

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
