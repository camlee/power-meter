#include "energy_cycle.h"

#include "historical_storage.h"
#include "../device/device_state.h"
#include "../time/time_service.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <algorithm>

#include "memory/heap_policy.h"

namespace energy_cycle {
namespace {
constexpr char kPreferencesNamespace[] = "energy_cycle";
constexpr char kEndHourKey[] = "end_hour";
constexpr int64_t kMinuteMs = 60LL * 1000LL;
constexpr int64_t kHourMs = 60LL * kMinuteMs;
constexpr int64_t kDayMs = 24LL * kHourMs;

uint8_t configuredEndHour = kDefaultEndHour;
bool initialized = false;

int64_t floorDay(int64_t localUnixMs) {
    // Accepted wall times are positive, but keep this correct for any epoch.
    int64_t day = localUnixMs / kDayMs;
    if (localUnixMs < 0 && localUnixMs % kDayMs) --day;
    return day * kDayMs;
}
} // namespace

void init() {
    if (initialized) return;
    Preferences preferences;
    if (preferences.begin(kPreferencesNamespace, true)) {
        const uint8_t saved = preferences.getUChar(kEndHourKey, kDefaultEndHour);
        configuredEndHour = saved < 24 ? saved : kDefaultEndHour;
        preferences.end();
    }
    initialized = true;
}

uint8_t endHour() {
    init();
    return configuredEndHour;
}

bool setEndHour(uint8_t hour) {
    if (hour >= 24) return false;
    init();
    if (hour == configuredEndHour) return true;
    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) return false;
    const bool saved = preferences.putUChar(kEndHourKey, hour) == sizeof(hour);
    preferences.end();
    if (!saved) return false;
    configuredEndHour = hour;
    device_state::changed(device_state::Domain::History);
    return true;
}

size_t query(Summary* out, size_t maxSummaries) {
    init();
    if (!out || !maxSummaries || !time_service::hasCurrentTime()) return 0;

    int64_t nowMs = 0;
    if (!time_service::resolveUnixTimeMs(time_service::currentSessionId(),
            time_service::monotonicUs(), nowMs)) return 0;

    const int64_t offsetMs = static_cast<int64_t>(time_service::utcOffsetMinutes()) * kMinuteMs;
    const int64_t localNowMs = nowMs + offsetMs;
    const int64_t todayBoundaryLocal = floorDay(localNowMs) +
                                       static_cast<int64_t>(configuredEndHour) * kHourMs;
    // At the boundary instant a new cycle has begun.
    const int64_t currentEndLocal = localNowMs < todayBoundaryLocal
        ? todayBoundaryLocal : todayBoundaryLocal + kDayMs;
    const int64_t currentEndMs = currentEndLocal - offsetMs;

    const size_t count = std::min(maxSummaries, kRecentCycleCount);
    const int64_t startMs = currentEndMs - static_cast<int64_t>(count) * kDayMs;
    auto* buckets = static_cast<historical_storage::PowerBucket*>(
        heap_policy::callocPreferred(count, sizeof(historical_storage::PowerBucket)));
    if (!buckets) return 0;
    historical_storage::QueryStatus status{};
    const size_t bucketCount = historical_storage::getTimePowerBuckets(
        buckets, count, startMs, currentEndMs, 24 * 60, &status);
    if (bucketCount != count) {
        heap_caps_free(buckets);
        return 0;
    }

    for (size_t i = 0; i < count; ++i) {
        Summary& summary = out[i];
        summary = {};
        summary.startUnixMs = buckets[i].startTimeMs;
        summary.endUnixMs = summary.startUnixMs + kDayMs;
        summary.current = i + 1 == count;
        const int64_t measuredEnd = std::min(nowMs, summary.endUnixMs);
        summary.expectedMs = measuredEnd > summary.startUnixMs
            ? static_cast<uint32_t>(measuredEnd - summary.startUnixMs) : 0;

        summary.validCoverageMs = std::min(
            buckets[i].coveredMs,
            std::min(buckets[i].channelCoverageMs[0], buckets[i].channelCoverageMs[1]));
        const uint64_t coverageScaled = static_cast<uint64_t>(summary.validCoverageMs) * 100;
        const uint64_t expectedScaled = static_cast<uint64_t>(summary.expectedMs);
        const bool enoughCoverage = summary.expectedMs != 0 &&
            coverageScaled >= expectedScaled * kMinimumAvailablePercent;
        summary.chargeAvailable = enoughCoverage;
        summary.useAvailable = enoughCoverage;
        summary.netAvailable = enoughCoverage;
        summary.chargeWh = buckets[i].energyWh[0] * kChargeEfficiency;
        summary.useWh = buckets[i].energyWh[1];
        summary.netWh = summary.chargeWh - summary.useWh;
        summary.qualityFlags = buckets[i].qualityFlags;
        summary.incomplete = enoughCoverage &&
            coverageScaled < expectedScaled * kCompleteCoveragePercent;
    }
    heap_caps_free(buckets);
    return count;
}

} // namespace energy_cycle
