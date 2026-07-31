import test from 'node:test';
import assert from 'node:assert/strict';
import {
  calibrationAxis,
  fixedSensorAxis,
  sensorAxis,
} from '../src/lib/sensorAxis.js';

test('sensor axes use whole-number nice increments and LVGL default windows', () => {
  assert.deepEqual(sensorAxis('voltage', [13.2, 13.3]), {
    low: 12, high: 14, step: 1,
  });
  assert.deepEqual(sensorAxis('current', [0.1, 0.2]), {
    low: 0, high: 6, step: 2,
  });
  assert.deepEqual(sensorAxis('voltage', [24.2]), {
    low: 10, high: 25, step: 5,
  });
  assert.deepEqual(sensorAxis('power', [100]), {
    low: 99, high: 101, step: 1,
  });
});

test('calibration axes include zero and use the LVGL nice-step approach', () => {
  assert.deepEqual(calibrationAxis([13.1, 13.4], 'voltage'), {
    low: 0, high: 15, step: 5,
  });
  assert.deepEqual(calibrationAxis([-7.2, -6.8], 'current'), {
    low: -10, high: 0, step: 2,
  });
});

test('fixed ADC axes keep their exact domain with intuitive ticks', () => {
  assert.deepEqual(fixedSensorAxis(0, 3.3), {
    low: 0, high: 3.3, step: 1, ticks: [0, 1, 2, 3],
  });
});
