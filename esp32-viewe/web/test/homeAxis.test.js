import assert from 'node:assert/strict';
import test from 'node:test';

import { homeAxis } from '../src/lib/homeAxis.js';

test('home axis always includes a labeled zero with a one-watt small scale', () => {
  assert.deepEqual(homeAxis(1.2, 4.1), { low: 0, high: 5, step: 1 });
  assert.deepEqual(homeAxis(-4.1, -1.2), { low: -5, high: 0, step: 1 });
});

test('home axis uses readable nice steps as power grows', () => {
  assert.deepEqual(homeAxis(0, 20), { low: 0, high: 20, step: 5 });
  assert.deepEqual(homeAxis(-83, 12), { low: -100, high: 20, step: 20 });
  assert.deepEqual(homeAxis(0, 140), { low: 0, high: 150, step: 50 });
});

test('home axis has a stable waiting scale', () => {
  assert.deepEqual(homeAxis(Number.NaN, Number.NaN), {
    low: -1, high: 1, step: 1,
  });
});
