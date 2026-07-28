import test from 'node:test';
import assert from 'node:assert/strict';

import {
  apDraftFromDevice,
  apDraftMatchesDevice,
  apRequestFromDraft,
  stationSelection,
} from '../src/lib/wifiDraft.js';

test('saved AP metadata initializes a blank, non-replacement draft', () => {
  const draft = apDraftFromDevice({
    enabled: true,
    ssid: 'meter-test',
    secure: true,
    password_configured: true,
  });
  assert.deepEqual(draft, {
    enabled: true,
    ssid: 'meter-test',
    secure: true,
    passwordConfigured: true,
    replacingPassword: false,
    password: '',
  });
  assert.equal(Object.hasOwn(draft, 'savedPassword'), false);
});

test('AP requests express keep, replace, and remove without accidental values', () => {
  const base = apDraftFromDevice({
    enabled: true, ssid: 'meter-test', secure: true, password_configured: true,
  });
  assert.deepEqual(apRequestFromDraft(base), {
    enabled: true, ssid: 'meter-test', secure: true, password_action: 'keep',
  });
  assert.deepEqual(apRequestFromDraft({
    ...base, replacingPassword: true, password: 'test-only-password',
  }), {
    enabled: true, ssid: 'meter-test', secure: true,
    password_action: 'replace', password: 'test-only-password',
  });
  assert.deepEqual(apRequestFromDraft({
    ...base, secure: false, password: '',
  }), {
    enabled: true, ssid: 'meter-test', secure: false, password_action: 'remove',
  });
  assert.deepEqual(apRequestFromDraft({ ...base, enabled: false }), {
    enabled: false,
  });
});

test('local password editing prevents poll synchronization', () => {
  const ap = {
    enabled: false, ssid: 'meter-test', secure: true, password_configured: true,
  };
  const draft = apDraftFromDevice(ap);
  assert.equal(apDraftMatchesDevice(draft, ap), true);
  assert.equal(apDraftMatchesDevice({
    ...draft, replacingPassword: true, password: 'test-only-password',
  }, ap), false);
});

test('selecting another station always clears its password draft', () => {
  assert.deepEqual(stationSelection({ ssid: 'other', secure: true }), {
    ssid: 'other', secure: true, password: '',
  });
});
