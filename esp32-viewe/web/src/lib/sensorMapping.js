export const SENSOR_ROLES = ['solar', 'load', 'battery', 'unmapped'];
export const CURRENT_DIRECTIONS = ['normal', 'reversed'];

export function cloneSensorMapping(mapping) {
  if (!mapping) return null;
  return {
    ...mapping,
    physical_sensors: (mapping.physical_sensors || []).map((sensor) => ({
      ...sensor,
      calibration: sensor.calibration
        ? {
          ...sensor.calibration,
          voltage: { ...sensor.calibration.voltage },
          current: { ...sensor.calibration.current },
        }
        : null,
    })),
  };
}

export function mappingIsValid(mapping) {
  const sensors = mapping?.physical_sensors;
  if (!Array.isArray(sensors) || sensors.length !== 3) return false;
  if (sensors.some((sensor) =>
    !SENSOR_ROLES.includes(sensor.role) ||
    !CURRENT_DIRECTIONS.includes(sensor.current_direction))) return false;
  const count = (role) => sensors.filter((sensor) => sensor.role === role).length;
  return count('solar') === 1 && count('load') === 1 && count('battery') <= 1;
}

export function mappingsEqual(left, right) {
  if (!left || !right || left.source !== right.source ||
      (left.balance_visible === true) !== (right.balance_visible === true)) {
    return false;
  }
  const leftSensors = left.physical_sensors || [];
  const rightSensors = right.physical_sensors || [];
  return leftSensors.length === rightSensors.length &&
    leftSensors.every((sensor, index) =>
      sensor.id === rightSensors[index]?.id &&
      sensor.role === rightSensors[index]?.role &&
      sensor.current_direction === rightSensors[index]?.current_direction);
}

export function directionMultiplier(direction) {
  return direction === 'reversed' ? -1 : 1;
}

function interpretation(role, power, available) {
  const label = {
    solar: 'Solar', load: 'Load', battery: 'Battery', unmapped: 'None',
  }[role] || 'None';
  if (role === 'unmapped') return { text: 'None: Not mapped', warning: false };
  if (!available || !Number.isFinite(power)) {
    return { text: `${label}: Waiting for reading`, warning: false };
  }
  if (Math.abs(power) < 0.5) {
    return { text: `${label}: Idle`, warning: false };
  }
  if (role === 'solar') {
    return power > 0
      ? { text: 'Solar: Producing', warning: false }
      : { text: 'Solar: Consuming — check polarity', warning: true };
  }
  if (role === 'load') {
    return power > 0
      ? { text: 'Load: Consuming', warning: false }
      : { text: 'Load: Producing — check polarity', warning: true };
  }
  return power > 0
    ? { text: 'Battery: Charging', warning: false }
    : { text: 'Battery: Discharging', warning: false };
}

export function previewSensorMapping(saved, draft, livePhysicalSensors = null) {
  const liveById = new Map(
    (livePhysicalSensors || []).map((sensor) => [sensor.id, sensor]),
  );
  return (draft?.physical_sensors || []).map((draftSensor, index) => {
    const savedSensor = saved?.physical_sensors?.[index] || draftSensor;
    const reading = liveById.get(draftSensor.id) || savedSensor;
    const ratio = directionMultiplier(draftSensor.current_direction) /
      directionMultiplier(savedSensor.current_direction);
    const available = reading?.observed !== false &&
      Number.isFinite(reading?.power);
    const voltage = Number.isFinite(reading?.voltage)
      ? reading.voltage : Number.NaN;
    const current = Number.isFinite(reading?.current)
      ? reading.current * ratio : Number.NaN;
    const power = Number.isFinite(reading?.power)
      ? reading.power * ratio : Number.NaN;
    return {
      ...draftSensor,
      label: savedSensor.label || draftSensor.label,
      configured: reading?.configured === true ||
        reading?.source_configured === true,
      observed: reading?.observed === true,
      eligible: reading?.eligible === true,
      state: reading?.state || 'waiting',
      voltage,
      current,
      power,
      available,
      interpretation: interpretation(draftSensor.role, power, available),
    };
  });
}

export function mappingBalance(draft, preview) {
  if (!mappingIsValid(draft)) {
    return {
      available: false,
      value: Number.NaN,
      percentage: Number.NaN,
      help: 'Unavailable — assign Solar and Load once',
    };
  }
  if (!(draft.physical_sensors || []).some((sensor) => sensor.role === 'battery')) {
    return {
      available: false,
      value: Number.NaN,
      percentage: Number.NaN,
      help: 'Unavailable — map Battery to calculate',
    };
  }
  const powerFor = (role) => preview.find((sensor) => sensor.role === role);
  const solar = powerFor('solar');
  const load = powerFor('load');
  const battery = powerFor('battery');
  if (![solar, load, battery].every((sensor) =>
    sensor?.available && Number.isFinite(sensor.power))) {
    return {
      available: false,
      value: Number.NaN,
      percentage: Number.NaN,
      help: 'Unavailable — waiting for all three readings',
    };
  }
  const value = solar.power - load.power - battery.power;
  const largest = Math.max(
    Math.abs(solar.power), Math.abs(load.power), Math.abs(battery.power),
  );
  return {
    available: true,
    value,
    percentage: largest > 0.05 ? Math.abs(value) * 100 / largest : 0,
    help: 'Unaccounted power',
  };
}
