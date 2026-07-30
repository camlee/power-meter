#pragma once

#include <cmath>

namespace home_axis {

struct Scale {
    float minimum;
    float maximum;
    float step;
};

inline float niceStep(float target) {
    target = std::fmax(target, 1.0f);
    const float magnitude =
        std::pow(10.0f, std::floor(std::log10(target)));
    const float normalized = target / magnitude;
    const float base = normalized <= 1.0f ? 1.0f :
                       normalized <= 2.0f ? 2.0f :
                       normalized <= 5.0f ? 5.0f : 10.0f;
    return base * magnitude;
}

inline Scale scale(float observedMinimum, float observedMaximum) {
    if (!std::isfinite(observedMinimum) ||
        !std::isfinite(observedMaximum)) {
        return {-1.0f, 1.0f, 1.0f};
    }
    const float low = std::fmin(observedMinimum, 0.0f);
    const float high = std::fmax(observedMaximum, 0.0f);
    const float step = niceStep((high - low) / 5.0f);
    float minimum = std::floor(low / step) * step;
    float maximum = std::ceil(high / step) * step;
    if (maximum <= minimum) {
        minimum = -step;
        maximum = step;
    }
    return {minimum, maximum, step};
}

} // namespace home_axis
