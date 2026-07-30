#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace home_kpi {

struct Result {
    bool publish = false;
    bool available = false;
    float value = 0.0f;
};

// Keeps naturally noisy readings stable without hiding a real step change.
// Normal updates publish a trailing four-sample average every two seconds.
// Two consecutive samples beyond the change threshold publish early.
class AdaptiveFilter {
public:
    Result add(uint32_t nowMs, float sample) {
        samples_[next_] = sample;
        next_ = (next_ + 1) % kWindow;
        count_ = std::min(count_ + 1, kWindow);

        if (!initialized_) {
            initialized_ = true;
            lastPublishMs_ = nowMs;
            displayedAvailable_ = std::isfinite(sample);
            displayed_ = displayedAvailable_ ? sample : 0.0f;
            return {displayedAvailable_, displayedAvailable_, displayed_};
        }

        const bool significant =
            std::isfinite(sample)
                ? (!displayedAvailable_ ||
                   std::fabs(sample - displayed_) >=
                       std::max(kMinimumStepWatts,
                                std::fabs(displayed_) * kRelativeStep))
                : displayedAvailable_;
        significantCount_ = significant ? significantCount_ + 1 : 0;

        if (significantCount_ >= 2) {
            significantCount_ = 0;
            return publish(nowMs, 2);
        }
        if (static_cast<uint32_t>(nowMs - lastPublishMs_) >=
            kRegularIntervalMs) {
            significantCount_ = 0;
            return publish(nowMs, kWindow);
        }
        return {};
    }

    void reset() { *this = AdaptiveFilter{}; }

private:
    static constexpr size_t kWindow = 4;
    static constexpr uint32_t kRegularIntervalMs = 2000;
    static constexpr float kMinimumStepWatts = 5.0f;
    static constexpr float kRelativeStep = 0.05f;

    Result publish(uint32_t nowMs, size_t requested) {
        float total = 0.0f;
        size_t finiteCount = 0;
        const size_t used = std::min(requested, count_);
        for (size_t offset = 0; offset < used; ++offset) {
            const size_t index =
                (next_ + kWindow - 1 - offset) % kWindow;
            if (std::isfinite(samples_[index])) {
                total += samples_[index];
                ++finiteCount;
            }
        }
        lastPublishMs_ = nowMs;
        displayedAvailable_ = finiteCount > 0;
        displayed_ = displayedAvailable_
            ? total / static_cast<float>(finiteCount) : 0.0f;
        return {true, displayedAvailable_, displayed_};
    }

    float samples_[kWindow]{};
    size_t next_ = 0;
    size_t count_ = 0;
    uint8_t significantCount_ = 0;
    uint32_t lastPublishMs_ = 0;
    float displayed_ = 0.0f;
    bool initialized_ = false;
    bool displayedAvailable_ = false;
};

} // namespace home_kpi
