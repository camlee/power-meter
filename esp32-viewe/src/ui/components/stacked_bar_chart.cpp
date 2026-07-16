#include "stacked_bar_chart.h"

#include <cmath>
#include <ctime>

namespace stacked_bar_chart {
namespace {

struct State { Data data{}; float min = -1; float max = 1; float step = 1; };
constexpr int kLeft = 42, kRight = 4, kTop = 18, kBottom = 24;

float niceStep(float value) {
    if (value <= 0) return 1;
    const float power = powf(10, floorf(log10f(value)));
    const float unit = value / power;
    return (unit <= 1 ? 1 : unit <= 2 ? 2 : unit <= 5 ? 5 : 10) * power;
}
void relativeLabel(char* out, size_t size, uint32_t minutes) {
    if (!minutes) lv_snprintf(out, size, "now");
    else if (minutes < 60) lv_snprintf(out, size, "-%um", (unsigned)minutes);
    else if (minutes < 1440) lv_snprintf(out, size, "-%uh", (unsigned)(minutes / 60));
    else lv_snprintf(out, size, "-%ud", (unsigned)(minutes / 1440));
}
void calendarLabel(char* out, size_t size, int64_t unixMs, uint32_t durationMinutes,
                   uint32_t tickMinutes, int16_t utcOffsetMinutes) {
    const time_t localSeconds = static_cast<time_t>(
        (unixMs + static_cast<int64_t>(utcOffsetMinutes) * 60000LL) / 1000LL);
    struct tm local{};
    gmtime_r(&localSeconds, &local);
    static constexpr const char* kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (durationMinutes > 2 * 1440 && tickMinutes >= 1440) {
        lv_snprintf(out, size, "%s", kWeekdays[local.tm_wday]);
        return;
    }
    const int hour = local.tm_hour % 12 ? local.tm_hour % 12 : 12;
    const char* meridiem = local.tm_hour < 12 ? "AM" : "PM";
    if (local.tm_min != 0) {
        lv_snprintf(out, size, "%d:%02d", hour, local.tm_min);
    } else {
        lv_snprintf(out, size, "%d%s", hour, meridiem);
    }
}
int yFor(const State& state, int top, int height, float value) {
    return top + (int)lroundf((state.max - value) * height / (state.max - state.min));
}
void drawLabel(lv_draw_ctx_t* ctx, const char* text, int x, int y, lv_color_t color) {
    lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
    dsc.color = color; dsc.font = &lv_font_montserrat_14;
    lv_area_t area{(lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)(x + 90), (lv_coord_t)(y + 16)}; lv_draw_label(ctx, &dsc, &area, text, nullptr);
}
void drawCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t* obj = lv_event_get_target(event);
    auto* state = static_cast<State*>(lv_event_get_user_data(event));
    if (!state || !state->data.pointCount) return;
    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
    const lv_area_t& a = obj->coords;
    const int left = a.x1 + kLeft, right = a.x2 - kRight, top = a.y1 + kTop, bottom = a.y2 - kBottom;
    const int width = right - left + 1, height = bottom - top + 1;
    if (width < 2 || height < 2) return;

    lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line); line.width = 1; line.color = lv_palette_lighten(LV_PALETTE_GREY, 2);
    const int zeroY = yFor(*state, top, height, 0);
    for (float tick = state->min; tick <= state->max + state->step * .1f; tick += state->step) {
        const int y = yFor(*state, top, height, tick);
        lv_point_t p1{(lv_coord_t)left, (lv_coord_t)y}, p2{(lv_coord_t)right, (lv_coord_t)y};
        line.width = fabsf(tick) < state->step * .01f ? 2 : 1; lv_draw_line(ctx, &line, &p1, &p2);
        char label[16]; lv_snprintf(label, sizeof(label), "%d W", (int)lroundf(tick));
        drawLabel(ctx, label, a.x1 + 1, y - 7, lv_palette_main(LV_PALETTE_GREY));
    }
    if (state->data.yAxisTitle && state->data.yAxisTitle[0]) {
        drawLabel(ctx, state->data.yAxisTitle, left, a.y1 + 1, lv_palette_main(LV_PALETTE_GREY));
    }

    const uint32_t tickMinutes = state->data.tickMinutes ? state->data.tickMinutes : state->data.durationMinutes;
    if (tickMinutes && state->data.durationMinutes) {
        constexpr int minLabelSpacing = 38;
        constexpr int labelGap = 4;
        const int plotSpan = right - left;
        int previousLabelRight = a.x1 - labelGap;
        if (state->data.axisStartUnixMs) {
            const int64_t minuteMs = 60000LL;
            const int64_t tickMs = static_cast<int64_t>(tickMinutes) * minuteMs;
            const int64_t durationMs = static_cast<int64_t>(state->data.durationMinutes) * minuteMs;
            const int64_t axisEnd = state->data.axisStartUnixMs + durationMs;
            const int64_t offsetMs = static_cast<int64_t>(state->data.utcOffsetMinutes) * minuteMs;
            const int64_t localStart = state->data.axisStartUnixMs + offsetMs;
            const int64_t firstLocalTick = ((localStart + tickMs - 1) / tickMs) * tickMs;
            const int64_t firstTick = firstLocalTick - offsetMs;
            const uint32_t tickCount = firstTick <= axisEnd
                ? static_cast<uint32_t>((axisEnd - firstTick) / tickMs) + 1 : 0;
            const int tickPixels = static_cast<int>((static_cast<int64_t>(plotSpan) * tickMs) / durationMs);
            const uint32_t labelEvery = tickPixels > 0
                ? static_cast<uint32_t>((minLabelSpacing + tickPixels - 1) / tickPixels) : 1;
            for (uint32_t tick = 0; tick < tickCount; ++tick) {
                const int64_t tickUnixMs = firstTick + static_cast<int64_t>(tick) * tickMs;
                if (tick % labelEvery != 0) continue;
                const int x = left + static_cast<int>(
                    static_cast<int64_t>(plotSpan) * (tickUnixMs - state->data.axisStartUnixMs) / durationMs);
                lv_point_t p1{(lv_coord_t)x, (lv_coord_t)top}, p2{(lv_coord_t)x, (lv_coord_t)bottom};
                line.width = 1;
                lv_draw_line(ctx, &line, &p1, &p2);
                char label[16];
                calendarLabel(label, sizeof(label), tickUnixMs, state->data.durationMinutes, tickMinutes,
                              state->data.utcOffsetMinutes);
                const int labelWidth = lv_txt_get_width(label, strlen(label), &lv_font_montserrat_14, 0,
                                                        LV_TEXT_FLAG_NONE);
                const int labelX = std::max(a.x1 + 1, std::min(x - labelWidth / 2,
                                                               right - labelWidth + 1));
                if (labelX < previousLabelRight + labelGap) continue;
                drawLabel(ctx, label, labelX, bottom + 5, lv_palette_main(LV_PALETTE_GREY));
                previousLabelRight = labelX + labelWidth;
            }
        } else {
            const uint32_t tickCount = state->data.durationMinutes / tickMinutes;
            if (tickCount) for (uint32_t tick = 0; tick <= tickCount; ++tick) {
                const int x = left + static_cast<int>(static_cast<int64_t>(plotSpan) * tick / tickCount);
                lv_point_t p1{(lv_coord_t)x, (lv_coord_t)top}, p2{(lv_coord_t)x, (lv_coord_t)bottom};
                line.width = 1;
                lv_draw_line(ctx, &line, &p1, &p2);
                char label[12];
                relativeLabel(label, sizeof(label), state->data.durationMinutes - tick * tickMinutes);
                const int labelWidth = lv_txt_get_width(label, strlen(label), &lv_font_montserrat_14, 0,
                                                        LV_TEXT_FLAG_NONE);
                const int labelX = std::max(a.x1 + 1, std::min(x - labelWidth / 2,
                                                               right - labelWidth + 1));
                if (labelX < previousLabelRight + labelGap) continue;
                drawLabel(ctx, label, labelX, bottom + 5, lv_palette_main(LV_PALETTE_GREY));
                previousLabelRight = labelX + labelWidth;
            }
        }
    }

    lv_draw_rect_dsc_t rect; lv_draw_rect_dsc_init(&rect); rect.bg_opa = LV_OPA_COVER; rect.radius = 0;
    for (size_t point = 0; point < state->data.pointCount; ++point) {
        const int x1 = left + (int)((int64_t)width * point / state->data.pointCount);
        const int next = left + (int)((int64_t)width * (point + 1) / state->data.pointCount);
        const int x2 = next - (next - x1 >= 3 ? 2 : 1); // fixed one-pixel gap where physically possible
        if (x2 < x1) continue;
        float positive = 0, negative = 0;
        for (uint8_t series = 0; series < state->data.seriesCount; ++series) {
            const Series& s = state->data.series[series]; const float value = s.values ? s.values[point] : 0;
            if (!std::isfinite(value) || value <= 0) continue;
            const float from = s.positive ? positive : -negative;
            const float to = s.positive ? positive + value : -(negative + value);
            const int yFrom = yFor(*state, top, height, from);
            const int yTo = yFor(*state, top, height, to);
            lv_area_t bar{(lv_coord_t)x1, (lv_coord_t)(yFrom < yTo ? yFrom : yTo),
                          (lv_coord_t)x2, (lv_coord_t)(yFrom > yTo ? yFrom : yTo)};
            rect.bg_color = s.color; lv_draw_rect(ctx, &rect, &bar);
            if (s.positive) positive += value; else negative += value;
        }
    }
    (void)zeroY;
}
} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent); lv_obj_remove_style_all(obj);
    lv_obj_set_width(obj, lv_pct(100)); lv_obj_set_flex_grow(obj, 1);
    auto* state = new State(); lv_obj_set_user_data(obj, state); lv_obj_add_event_cb(obj, drawCb, LV_EVENT_DRAW_MAIN, state);
    return obj;
}
void setData(lv_obj_t* chart, const Data& data) {
    auto* state = static_cast<State*>(lv_obj_get_user_data(chart));
    state->data = data;
    float high = 0, low = 0;
    for (size_t point = 0; point < data.pointCount; ++point) {
        float up = 0, down = 0;
        for (uint8_t s = 0; s < data.seriesCount; ++s) {
            const float value = data.series[s].values[point];
            if (!std::isfinite(value)) continue;
            (data.series[s].positive ? up : down) += value;
        }
        high = fmaxf(high, up); low = fmaxf(low, down);
    }
    state->step = niceStep(fmaxf(high + low, 1) / 6); state->max = ceilf(high / state->step) * state->step;
    state->min = -ceilf(low / state->step) * state->step;
    if (state->max == 0) state->max = state->step; if (state->min == 0) state->min = -state->step;
    lv_obj_invalidate(chart);
}
} // namespace stacked_bar_chart
