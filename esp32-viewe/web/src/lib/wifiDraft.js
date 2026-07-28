export function apDraftFromDevice(ap = {}) {
  return {
    enabled: !!ap.enabled,
    ssid: ap.ssid || '',
    secure: !!ap.secure,
    passwordConfigured: !!ap.password_configured,
    replacingPassword: false,
    password: '',
  };
}

export function apDraftMatchesDevice(draft, ap) {
  return !!draft && !!ap && !draft.replacingPassword && draft.password === '' &&
    draft.enabled === !!ap.enabled && draft.ssid === (ap.ssid || '') &&
    draft.secure === !!ap.secure &&
    draft.passwordConfigured === !!ap.password_configured;
}

export function apRequestFromDraft(draft) {
  if (!draft.enabled) return { enabled: false };
  if (!draft.secure) {
    return {
      enabled: true,
      ssid: draft.ssid,
      secure: false,
      password_action: 'remove',
    };
  }
  if (draft.replacingPassword) {
    return {
      enabled: true,
      ssid: draft.ssid,
      secure: true,
      password_action: 'replace',
      password: draft.password,
    };
  }
  return {
    enabled: true,
    ssid: draft.ssid,
    secure: true,
    password_action: 'keep',
  };
}

export function stationSelection(network) {
  return {
    ssid: network?.ssid || '',
    secure: !!network?.secure,
    password: '',
  };
}
