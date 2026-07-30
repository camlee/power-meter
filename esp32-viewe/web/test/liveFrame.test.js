import test from 'node:test';
import assert from 'node:assert/strict';
import { parseLiveFrame } from '../src/lib/api.js';

function v5Frame() {
  const buffer = new ArrayBuffer(128);
  const view = new DataView(buffer);
  view.setUint32(0, 0x354d5056, true);
  view.setUint8(4, 5);
  view.setUint8(5, 1);
  view.setUint16(6, 0x03fe, true);
  view.setUint32(8, 42, true);
  view.setUint32(12, 7, true);
  view.setUint32(16, 1234, true);
  view.setUint32(20, 99, true);
  view.setFloat64(24, 123456.5, true);
  [13, 2, 26, 13, 1, 13, 13, -1, -13, 13, 1, 1, 1].forEach(
    (value, index) => view.setFloat32(32 + index * 4, value, true),
  );
  view.setUint16(84, 0x01ff, true);
  view.setUint8(86, 2);
  view.setUint8(87, 3);
  view.setUint8(88, 1);
  [13.1, 13.2, 13.3].forEach(
    (value, index) => view.setFloat32(92 + index * 4, value, true),
  );
  [2.1, 2.2, 2.3].forEach(
    (value, index) => view.setFloat32(104 + index * 4, value, true),
  );
  [27.5, 29, 30.5].forEach(
    (value, index) => view.setFloat32(116 + index * 4, value, true),
  );
  return buffer;
}

test('parses V5 physical diagnostics without changing logical offsets', () => {
  const frame = parseLiveFrame(v5Frame());
  assert.equal(frame.version, 5);
  assert.equal(frame.sequence, 42);
  assert.equal(frame.in.power, 26);
  assert.equal(frame.aux.power, -13);
  assert.equal(frame.physicalSensors[0].id, 'sensor1');
  assert.equal(frame.physicalSensors[0].state, 'valid');
  assert.equal(frame.physicalSensors[1].state, 'out_of_range');
  assert.equal(frame.physicalSensors[2].state, 'waiting');
  assert.ok(Math.abs(frame.physicalSensors[2].power - 30.5) < 0.0001);
});

test('continues to accept compact V4 replay frames', () => {
  const buffer = v5Frame().slice(0, 84);
  const view = new DataView(buffer);
  view.setUint32(0, 0x344d5056, true);
  view.setUint8(4, 4);
  const frame = parseLiveFrame(buffer);
  assert.equal(frame.version, 4);
  assert.equal(frame.in.power, 26);
  assert.equal(frame.physicalSensors, null);
});

test('rejects malformed live frames', () => {
  assert.equal(parseLiveFrame(new ArrayBuffer(12)), null);
  const frame = v5Frame();
  new DataView(frame).setUint32(0, 0, true);
  assert.equal(parseLiveFrame(frame), null);
});
