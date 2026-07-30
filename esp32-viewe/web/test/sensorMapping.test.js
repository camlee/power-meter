import test from 'node:test';
import assert from 'node:assert/strict';
import {
  cloneSensorMapping,
  mappingBalance,
  mappingIsValid,
  mappingsEqual,
  previewSensorMapping,
} from '../src/lib/sensorMapping.js';

function mapping(entries = [
  ['sensor1', 'solar', 'normal'],
  ['sensor2', 'load', 'normal'],
  ['sensor3', 'battery', 'reversed'],
]) {
  return {
    source: 'adc',
    balance_visible: false,
    physical_sensors: entries.map(([id, role, direction], index) => ({
      id,
      label: `Sensor ${index + 1}`,
      role,
      current_direction: direction,
      configured: true,
      observed: true,
      eligible: true,
      state: 'valid',
      voltage: 13 + index,
      current: 2 + index,
      power: [40, 20, 15][index],
    })),
  };
}

test('validates required roles while allowing an unmapped Battery', () => {
  assert.equal(mappingIsValid(mapping()), true);
  assert.equal(mappingIsValid(mapping([
    ['sensor1', 'solar', 'normal'],
    ['sensor2', 'load', 'normal'],
    ['sensor3', 'unmapped', 'normal'],
  ])), true);
  assert.equal(mappingIsValid(mapping([
    ['sensor1', 'solar', 'normal'],
    ['sensor2', 'solar', 'normal'],
    ['sensor3', 'battery', 'normal'],
  ])), false);
});

test('clone and equality compare only persisted mapping choices', () => {
  const saved = mapping();
  const draft = cloneSensorMapping(saved);
  draft.physical_sensors[0].power = 999;
  assert.equal(mappingsEqual(saved, draft), true);
  draft.physical_sensors[0].role = 'load';
  assert.equal(mappingsEqual(saved, draft), false);
  assert.equal(saved.physical_sensors[0].role, 'solar');
  draft.physical_sensors[0].role = 'solar';
  draft.balance_visible = true;
  assert.equal(mappingsEqual(saved, draft), false);
});

test('draft polarity immediately reverses effective current and power', () => {
  const saved = mapping();
  const draft = cloneSensorMapping(saved);
  draft.physical_sensors[0].current_direction = 'reversed';
  const preview = previewSensorMapping(saved, draft, saved.physical_sensors);
  assert.equal(preview[0].current, -2);
  assert.equal(preview[0].power, -40);
  assert.equal(preview[0].interpretation.warning, true);
  assert.match(preview[0].interpretation.text, /Solar: Consuming/);
});

test('role changes immediately change the interpretation', () => {
  const saved = mapping();
  const draft = cloneSensorMapping(saved);
  draft.physical_sensors[0].role = 'battery';
  const preview = previewSensorMapping(saved, draft, saved.physical_sensors);
  assert.equal(preview[0].interpretation.text, 'Battery: Charging');
});

test('Balance follows the complete draft mapping and polarity', () => {
  const saved = mapping();
  const draft = cloneSensorMapping(saved);
  const preview = previewSensorMapping(saved, draft, saved.physical_sensors);
  assert.deepEqual(mappingBalance(draft, preview), {
    available: true,
    value: 5,
    percentage: 12.5,
    help: 'Unaccounted power',
  });
});

test('Balance explains an optional unmapped Battery', () => {
  const saved = mapping([
    ['sensor1', 'solar', 'normal'],
    ['sensor2', 'load', 'normal'],
    ['sensor3', 'unmapped', 'normal'],
  ]);
  const preview = previewSensorMapping(saved, saved, saved.physical_sensors);
  const balance = mappingBalance(saved, preview);
  assert.equal(balance.available, false);
  assert.match(balance.help, /map Battery/);
});
