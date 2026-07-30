export function powerBalance(solar, load, battery) {
  return Number.isFinite(solar) && Number.isFinite(load) && Number.isFinite(battery)
    ? solar - load - battery
    : Number.NaN;
}

export function usageBreakdown(solar, load, battery, batteryMeasured = true) {
  const unavailableRange = () => ({ from: Number.NaN, to: Number.NaN });
  const segment = (from, to) => {
    if (!Number.isFinite(from) || !Number.isFinite(to) ||
        Math.abs(to - from) <= 0.0001) return unavailableRange();
    return {
      from: Object.is(from, -0) ? 0 : from,
      to: Object.is(to, -0) ? 0 : to,
    };
  };
  const haveSolar = Number.isFinite(solar);
  const haveLoad = Number.isFinite(load);
  const solarTotal = haveSolar ? Math.max(solar, 0) : Number.NaN;
  const loadTotal = haveLoad ? Math.max(load, 0) : Number.NaN;
  const result = {
    solarTotal,
    loadTotal,
    charge: Number.NaN,
    solarRemainder: solarTotal,
    loadRemainder: loadTotal,
    discharge: Number.NaN,
    balance: batteryMeasured ? powerBalance(solar, load, battery) : Number.NaN,
    solarSegment: haveSolar ? segment(0, solarTotal) : unavailableRange(),
    loadSegment: haveLoad ? segment(0, -loadTotal) : unavailableRange(),
    chargeSegment: unavailableRange(),
    dischargeSegment: unavailableRange(),
    balanceSegment: unavailableRange(),
    batteryMeasured,
    conflict: false,
  };
  if (!haveSolar || !haveLoad || !Number.isFinite(battery)) return result;

  result.charge = Math.max(battery, 0);
  result.discharge = Math.max(-battery, 0);
  const balanceIn = batteryMeasured ? Math.max(result.balance, 0) : 0;
  const balanceOut = batteryMeasured ? Math.max(-result.balance, 0) : 0;
  const solarDirect = solarTotal - result.charge - balanceIn;
  const loadDirect = loadTotal - result.discharge - balanceOut;
  const direct = (solarDirect + loadDirect) / 2;

  result.solarRemainder = Math.max(direct, 0);
  result.loadRemainder = result.solarRemainder;
  result.solarSegment = unavailableRange();
  result.loadSegment = unavailableRange();

  if (batteryMeasured && direct < -0.0001) {
    result.conflict = true;
    if (result.charge > 0) {
      result.loadSegment = segment(direct, direct - loadTotal);
      result.balanceSegment = segment(direct, loadTotal);
      result.chargeSegment = segment(loadTotal, loadTotal + result.charge);
    } else {
      result.dischargeSegment = segment(-solarTotal, -solarTotal - result.discharge);
      result.balanceSegment = segment(-solarTotal, result.discharge - loadTotal);
      result.solarSegment = segment(
        result.discharge - loadTotal,
        result.discharge - loadTotal + solarTotal,
      );
    }
    return result;
  }

  const normalDirect = Math.max(direct, 0);
  result.chargeSegment = segment(0, result.charge);
  result.solarSegment = segment(result.charge, result.charge + normalDirect);
  result.dischargeSegment = segment(0, -result.discharge);
  result.loadSegment = segment(
    -result.discharge,
    -result.discharge - normalDirect,
  );
  if (balanceIn > 0) {
    result.balanceSegment = segment(
      result.charge + normalDirect,
      result.charge + normalDirect + balanceIn,
    );
  } else if (balanceOut > 0) {
    result.balanceSegment = segment(
      -result.discharge - normalDirect,
      -result.discharge - normalDirect - balanceOut,
    );
  }
  return result;
}
