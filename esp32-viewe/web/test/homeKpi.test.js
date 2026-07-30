import assert from 'node:assert/strict';
import test from 'node:test';

import { AdaptiveHomeKpi } from '../src/lib/homeKpi.js';

test('Home KPI publishes a trailing four-point average every two seconds', () => {
  const filter = new AdaptiveHomeKpi();
  assert.equal(filter.add(0, 10).publish, true);
  assert.equal(filter.add(500, 11).publish, false);
  assert.equal(filter.add(1000, 9).publish, false);
  assert.equal(filter.add(1500, 10).publish, false);
  assert.deepEqual(filter.add(2000, 12), {
    publish: true, available: true, value: 10.5,
  });
});

test('Home KPI publishes a sustained step after two samples', () => {
  const filter = new AdaptiveHomeKpi();
  filter.add(0, 10);
  assert.equal(filter.add(500, 30).publish, false);
  assert.deepEqual(filter.add(1000, 32), {
    publish: true, available: true, value: 31,
  });
});

test('Home KPI ignores a single spike', () => {
  const filter = new AdaptiveHomeKpi();
  filter.add(0, 10);
  assert.equal(filter.add(500, 30).publish, false);
  assert.equal(filter.add(1000, 11).publish, false);
});

test('Home KPI keeps loading through the initial missing sample', () => {
  const filter = new AdaptiveHomeKpi();
  assert.equal(filter.add(0, Number.NaN).publish, false);
  assert.equal(filter.add(500, Number.NaN).publish, false);
  assert.deepEqual(filter.add(2000, Number.NaN), {
    publish: true, available: false, value: 0,
  });
});
