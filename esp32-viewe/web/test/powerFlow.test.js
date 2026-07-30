import test from 'node:test';
import assert from 'node:assert/strict';

import { powerBalance, usageBreakdown } from '../src/lib/powerFlow.js';

function assertTotals(flow) {
  assert.equal(flow.charge + flow.solarRemainder, flow.solarTotal);
  assert.equal(flow.loadRemainder + flow.discharge, flow.loadTotal);
}

test('charging is stacked inside measured Solar', () => {
  const flow = usageBreakdown(40, 14, 26);
  assert.equal(flow.solarTotal, 40);
  assert.equal(flow.loadTotal, 14);
  assert.equal(flow.charge, 26);
  assert.equal(flow.solarRemainder, 14);
  assert.equal(flow.loadRemainder, 14);
  assert.equal(flow.discharge, 0);
  assert.equal(flow.balance, 0);
  assert.deepEqual(flow.chargeSegment, { from: 0, to: 26 });
  assert.deepEqual(flow.solarSegment, { from: 26, to: 40 });
  assert.deepEqual(flow.loadSegment, { from: 0, to: -14 });
  assertTotals(flow);
});

test('discharging is stacked inside measured Load', () => {
  const flow = usageBreakdown(0, 16, -16);
  assert.equal(flow.loadRemainder, 0);
  assert.equal(flow.discharge, 16);
  assert.equal(flow.balance, 0);
  assert.deepEqual(flow.dischargeSegment, { from: 0, to: -16 });
  assertTotals(flow);
});

test('positive Balance is outermost after Charge and Solar', () => {
  const flow = usageBreakdown(40, 20, 15);
  assert.deepEqual(flow.chargeSegment, { from: 0, to: 15 });
  assert.deepEqual(flow.solarSegment, { from: 15, to: 35 });
  assert.deepEqual(flow.balanceSegment, { from: 35, to: 40 });
  assert.deepEqual(flow.loadSegment, { from: 0, to: -20 });
});

test('negative Balance is outermost after Load', () => {
  const flow = usageBreakdown(30, 20, 15);
  assert.deepEqual(flow.chargeSegment, { from: 0, to: 15 });
  assert.deepEqual(flow.solarSegment, { from: 15, to: 30 });
  assert.deepEqual(flow.loadSegment, { from: 0, to: -15 });
  assert.deepEqual(flow.balanceSegment, { from: -15, to: -20 });
});

test('charge conflict uses the full floating Balance band', () => {
  const flow = usageBreakdown(20, 6, 22);
  assert.equal(flow.charge, 22);
  assert.equal(flow.solarRemainder, 0);
  assert.equal(flow.balance, -8);
  assert.equal(flow.conflict, true);
  assert.deepEqual(flow.loadSegment, { from: -2, to: -8 });
  assert.deepEqual(flow.balanceSegment, { from: -2, to: 6 });
  assert.deepEqual(flow.chargeSegment, { from: 6, to: 28 });
});

test('excess discharge produces the mirrored floating conflict stack', () => {
  const flow = usageBreakdown(6, 20, -22);
  assert.equal(flow.balance, 8);
  assert.equal(flow.conflict, true);
  assert.deepEqual(flow.dischargeSegment, { from: -6, to: -28 });
  assert.deepEqual(flow.balanceSegment, { from: -6, to: 2 });
  assert.deepEqual(flow.solarSegment, { from: 2, to: 8 });
});

test('Balance is unavailable without all three measurements', () => {
  const flow = usageBreakdown(12, 5, Number.NaN);
  assert.equal(flow.solarRemainder, 12);
  assert.equal(flow.loadRemainder, 5);
  assert.equal(Number.isNaN(flow.balance), true);
  assert.equal(Number.isNaN(powerBalance(12, 5, Number.NaN)), true);
  assert.equal(Number.isNaN(flow.balanceSegment.from), true);
});

test('inferred Battery subdivides history without claiming Balance', () => {
  const flow = usageBreakdown(12, 5, 7, false);
  assert.equal(flow.charge, 7);
  assert.equal(flow.solarRemainder, 5);
  assert.equal(flow.loadRemainder, 5);
  assert.equal(Number.isNaN(flow.balance), true);
  assert.deepEqual(flow.chargeSegment, { from: 0, to: 7 });
  assert.deepEqual(flow.solarSegment, { from: 7, to: 12 });
  assert.deepEqual(flow.loadSegment, { from: 0, to: -5 });
  assert.equal(Number.isNaN(flow.balanceSegment.from), true);
  assertTotals(flow);
});
