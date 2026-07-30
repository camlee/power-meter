#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace power_flow {

struct SegmentRange {
    float from;
    float to;
};

struct UsageBreakdown {
    float solarTotal;
    float loadTotal;
    float charge;
    float solarRemainder;
    float loadRemainder;
    float discharge;
    float balance;
    SegmentRange solarSegment;
    SegmentRange loadSegment;
    SegmentRange chargeSegment;
    SegmentRange dischargeSegment;
    SegmentRange balanceSegment;
    bool batteryMeasured;
    bool conflict;
};

inline float unavailable() {
    return std::numeric_limits<float>::quiet_NaN();
}

inline SegmentRange unavailableRange() {
    return {unavailable(), unavailable()};
}

inline SegmentRange segment(float from, float to) {
    return std::isfinite(from) && std::isfinite(to) &&
           fabsf(to - from) > 0.0001f
        ? SegmentRange{from, to} : unavailableRange();
}

// Current pre-mapping contract:
//   Solar and Load are positive magnitudes.
//   Battery is positive while charging and negative while discharging.
// Balance is measurement disagreement, not another energy-flow component.
inline float balance(float solar, float load, float battery) {
    return std::isfinite(solar) && std::isfinite(load) && std::isfinite(battery)
        ? solar - load - battery : unavailable();
}

// `batteryMeasured` is false when old two-channel history supplies an inferred
// Battery flow. It can still subdivide the stacks, but cannot establish an
// independent three-sensor Balance.
inline UsageBreakdown usage(float solar, float load, float battery,
                            bool batteryMeasured = true,
                            bool showBalance = true) {
    const bool haveSolar = std::isfinite(solar);
    const bool haveLoad = std::isfinite(load);
    const float solarTotal = haveSolar ? std::max(solar, 0.0f) : unavailable();
    const float loadTotal = haveLoad ? std::max(load, 0.0f) : unavailable();

    UsageBreakdown result{
        solarTotal,
        loadTotal,
        unavailable(),
        solarTotal,
        loadTotal,
        unavailable(),
        batteryMeasured ? balance(solar, load, battery) : unavailable(),
        haveSolar ? segment(0.0f, solarTotal) : unavailableRange(),
        haveLoad ? segment(0.0f, -loadTotal) : unavailableRange(),
        unavailableRange(),
        unavailableRange(),
        unavailableRange(),
        batteryMeasured,
        false,
    };
    if (!haveSolar || !haveLoad || !std::isfinite(battery)) return result;

    result.charge = std::max(battery, 0.0f);
    result.discharge = std::max(-battery, 0.0f);
    if (!batteryMeasured || !showBalance) {
        // The normal user view preserves the measured Solar and Load totals
        // and treats Battery only as a subdivision of those bars. Diagnostic
        // conflict extensions are reserved for the Balance-visible view.
        result.charge = std::min(result.charge, solarTotal);
        result.discharge = std::min(result.discharge, loadTotal);
        result.solarRemainder = solarTotal - result.charge;
        result.loadRemainder = loadTotal - result.discharge;
        result.chargeSegment = segment(0.0f, result.charge);
        result.solarSegment = segment(result.charge, solarTotal);
        result.dischargeSegment = segment(0.0f, -result.discharge);
        result.loadSegment = segment(-result.discharge, -loadTotal);
        return result;
    }
    const float balanceIn = batteryMeasured ? std::max(result.balance, 0.0f) : 0.0f;
    const float balanceOut = batteryMeasured ? std::max(-result.balance, 0.0f) : 0.0f;
    const float solarDirect = solarTotal - result.charge - balanceIn;
    const float loadDirect = loadTotal - result.discharge - balanceOut;
    const float direct = (solarDirect + loadDirect) * 0.5f;

    result.solarRemainder = std::max(direct, 0.0f);
    result.loadRemainder = result.solarRemainder;
    result.solarSegment = unavailableRange();
    result.loadSegment = unavailableRange();

    if (batteryMeasured && direct < -0.0001f) {
        result.conflict = true;
        if (result.charge > 0.0f) {
            // Charge exceeds Solar while Load is also present. Keep every
            // magnitude intact in one floating stack:
            // Load | Balance (possibly across zero) | Charge.
            result.loadSegment = segment(direct, direct - loadTotal);
            result.balanceSegment = segment(direct, loadTotal);
            result.chargeSegment =
                segment(loadTotal, loadTotal + result.charge);
        } else {
            // Mirror case: discharge exceeds Load while Solar is present.
            result.dischargeSegment =
                segment(-solarTotal, -solarTotal - result.discharge);
            result.balanceSegment =
                segment(-solarTotal, result.discharge - loadTotal);
            result.solarSegment =
                segment(result.discharge - loadTotal,
                        result.discharge - loadTotal + solarTotal);
        }
        return result;
    }

    const float normalDirect = std::max(direct, 0.0f);
    // Match the established history presentation: Battery charge/discharge
    // touches zero, direct Solar/Load continues outward, and any ordinary
    // Balance residual occupies the outer tip.
    result.chargeSegment = segment(0.0f, result.charge);
    result.solarSegment =
        segment(result.charge, result.charge + normalDirect);
    result.dischargeSegment = segment(0.0f, -result.discharge);
    result.loadSegment =
        segment(-result.discharge, -result.discharge - normalDirect);
    if (balanceIn > 0.0f) {
        result.balanceSegment =
            segment(result.charge + normalDirect,
                    result.charge + normalDirect + balanceIn);
    } else if (balanceOut > 0.0f) {
        result.balanceSegment =
            segment(-result.discharge - normalDirect,
                    -result.discharge - normalDirect - balanceOut);
    }
    return result;
}

} // namespace power_flow
