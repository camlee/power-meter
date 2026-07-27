import test from 'node:test';
import assert from 'node:assert/strict';

import { projectLiveNow } from '../src/lib/liveTime.js';

test('projects a fresh point by its browser receipt age', () => {
  const point = {
    timestamp: 50_000,
    receivedAt: 1_000_000,
    receivedMonotonicAt: 5_000,
  };

  assert.equal(projectLiveNow(point, {
    wallNow: 1_000_500,
    monotonicNow: 5_500,
  }), 50_500);
});

test('ages a stale point outside a live chart window after remount', () => {
  const point = {
    timestamp: 50_000,
    receivedAt: 1_000_000,
    receivedMonotonicAt: 5_000,
  };
  const projectedNow = projectLiveNow(point, {
    wallNow: 1_090_000,
    monotonicNow: 95_000,
  });

  assert.ok(point.timestamp < projectedNow - 30_000);
});

test('prefers monotonic receipt time across a browser wall-clock correction', () => {
  const point = {
    timestamp: 50_000,
    receivedAt: 1_000_000,
    receivedMonotonicAt: 5_000,
  };

  assert.equal(projectLiveNow(point, {
    wallNow: 940_000,
    monotonicNow: 15_000,
  }), 60_000);
});

test('falls back to wall receipt time for existing points', () => {
  const point = { timestamp: 50_000, receivedAt: 1_000_000 };

  assert.equal(projectLiveNow(point, {
    wallNow: 1_004_000,
    monotonicNow: 10_000,
  }), 54_000);
});

test('does not project backward when a receipt clock appears to move backward', () => {
  const point = { timestamp: 50_000, receivedAt: 1_000_000 };

  assert.equal(projectLiveNow(point, {
    wallNow: 999_000,
    monotonicNow: 10_000,
  }), 50_000);
});
