#include "history_screen.h"

#include <cstdio>
#include <ctime>
#include <esp_heap_caps.h>

#include "memory/heap_policy.h"

#include "data/historical_storage.h"
#include "data/history_query_service.h"
#include "time/time_service.h"
#include "../../components/linear_progress.h"
#include "../../theme/ui_theme.h"

namespace history_screen {
namespace {

constexpr size_t kMaxVisibleFiles = 20;
lv_obj_t* summaryLabel = nullptr;
lv_obj_t* detailLabel = nullptr;
lv_obj_t* filesList = nullptr;
lv_obj_t* screenObject = nullptr;
lv_obj_t* progress = nullptr;
lv_obj_t* datasetTabview = nullptr;
historical_storage::HistoryFileInfo* fileBuffer = nullptr;
uint32_t pendingJob = 0;
bool loaded = false;
historical_storage::Dataset selectedDataset = historical_storage::Dataset::Real;

void updateDatasetTab() {
    if (!datasetTabview) return;
    lv_tabview_set_act(datasetTabview,
                       selectedDataset == historical_storage::Dataset::Demo ? 1 : 0,
                       LV_ANIM_OFF);
}

void formatMegabytes(char* out, size_t size, uint32_t bytes) {
    snprintf(out, size, "%.1f MB", bytes / (1024.0f * 1024.0f));
}

void appendDurationPart(char* out, size_t size, size_t& used, uint32_t value,
                        const char* singular, const char* plural) {
    if (!value || used >= size) return;
    const size_t available = size - used;
    const int written = snprintf(out + used, available, "%s%lu %s", used ? ", " : "",
                                 static_cast<unsigned long>(value), value == 1 ? singular : plural);
    if (written <= 0) return;
    used += static_cast<size_t>(written) >= available ? available - 1 : static_cast<size_t>(written);
}

void formatDuration(char* out, size_t size, uint32_t minutes, bool recorded) {
    if (!size) return;
    out[0] = '\0';
    const uint32_t days = minutes / (24 * 60);
    const uint32_t hours = (minutes / 60) % 24;
    size_t used = 0;
    appendDurationPart(out, size, used, days, "day", "days");
    appendDurationPart(out, size, used, hours, "hour", "hours");
    appendDurationPart(out, size, used, minutes % 60, "minute", "minutes");
    if (!used) snprintf(out, size, "%s", recorded ? "No recorded time" : "No recorded duration");
    else if (recorded && used + sizeof(" recorded") <= size) snprintf(out + used, size - used, " recorded");
}

void formatRecordedDuration(char* out, size_t size, uint32_t minutes) {
    formatDuration(out, size, minutes, true);
}

void formatFileDuration(char* out, size_t size, uint32_t minutes) {
    formatDuration(out, size, minutes, false);
}

void formatLocalTime(char* out, size_t size, int64_t unixMs) {
    const time_t localSeconds = static_cast<time_t>(
        (unixMs + static_cast<int64_t>(time_service::utcOffsetMinutes()) * 60000LL) / 1000LL);
    struct tm local{};
    gmtime_r(&localSeconds, &local);
    static constexpr const char* kMonths[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    const int hour = local.tm_hour % 12 ? local.tm_hour % 12 : 12;
    snprintf(out, size, "%s %d %d:%02d %s", kMonths[local.tm_mon], local.tm_mday,
             hour, local.tm_min, local.tm_hour < 12 ? "AM" : "PM");
}

lv_obj_t* addLine(lv_obj_t* parent, const char* text, lv_color_t color, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

void addFileCard(const historical_storage::HistoryFileInfo& file) {
    lv_obj_t* card = lv_obj_create(filesList);
    ui_theme::styleCard(card, 6);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    const uint32_t minutes = file.committedRecords + file.bufferedRecords;
    char duration[32];
    formatFileDuration(duration, sizeof(duration), minutes);

    lv_obj_t* titleRow = lv_obj_create(card);
    lv_obj_remove_style_all(titleRow);
    lv_obj_set_width(titleRow, lv_pct(100));
    lv_obj_set_height(titleRow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(titleRow, 4, 0);

    char text[128];
    snprintf(text, sizeof(text), "Session %lu%s", static_cast<unsigned long>(file.sessionId),
             file.state == historical_storage::FileState::Closed ? " " LV_SYMBOL_OK : "");
    lv_obj_t* session = lv_label_create(titleRow);
    lv_label_set_text(session, text);
    lv_obj_set_flex_grow(session, 1);
    lv_obj_set_style_text_color(session, ui_theme::text(), 0);
    lv_obj_set_style_text_font(session, &lv_font_montserrat_14, 0);

    lv_obj_t* durationLabel = lv_label_create(titleRow);
    lv_label_set_text(durationLabel, duration);
    lv_obj_set_style_text_color(durationLabel, ui_theme::mutedText(), 0);
    lv_obj_set_style_text_font(durationLabel, &lv_font_montserrat_12, 0);

    if (file.timeFlags & historical_storage::TIME_ANCHORED) {
        char begin[32], end[32];
        formatLocalTime(begin, sizeof(begin), file.startUnixMs);
        formatLocalTime(end, sizeof(end), file.endUnixMs);
        snprintf(text, sizeof(text), "%s - %s", begin, end);
        addLine(card, text, ui_theme::mutedText(), &lv_font_montserrat_12);
    }
}

void render(const historical_storage::StorageStats& stats, size_t total, size_t count) {
    const uint32_t minutes = stats.committedRecords + stats.bufferedRecords;
    char used[24], text[128];
    formatRecordedDuration(text, sizeof(text), minutes);
    lv_label_set_text(summaryLabel, text);
    formatMegabytes(used, sizeof(used), stats.committedBytes + stats.bufferedBytes);
    snprintf(text, sizeof(text), "%u/%u files  |  %s", stats.fileCount, stats.maxFiles, used);
    lv_label_set_text(detailLabel, text);

    lv_obj_clean(filesList);
    if (!count) {
        addLine(filesList, "No data files yet", ui_theme::mutedText(), &lv_font_montserrat_14);
        return;
    }
    for (size_t i = 0; i < count; ++i) addFileCard(fileBuffer[i]);
    if (total > count) {
        snprintf(text, sizeof(text), "%u older files available through the JSON API",
                 static_cast<unsigned>(total - count));
        addLine(filesList, text, ui_theme::mutedText(), &lv_font_montserrat_12);
    }
}

void startLoad() {
    if (loaded || !screenObject || !lv_obj_is_visible(screenObject)) return;
    if (!fileBuffer) {
        fileBuffer = static_cast<historical_storage::HistoryFileInfo*>(
            heap_policy::callocPreferred(kMaxVisibleFiles,
                                         sizeof(historical_storage::HistoryFileInfo)));
    }
    if (!fileBuffer) return;
    pendingJob = history_query_service::requestFilesForDataset(selectedDataset, kMaxVisibleFiles);
    if (!pendingJob) {
        lv_label_set_text(summaryLabel, "History service unavailable");
        return;
    }
    linear_progress::show(progress);
    lv_label_set_text(summaryLabel, "Loading data...");
}

void completionCb(lv_timer_t*) {
    if (!pendingJob || !screenObject || !lv_obj_is_visible(screenObject)) return;
    historical_storage::StorageStats stats{};
    size_t total = 0;
    size_t count = 0;
    if (!history_query_service::takeFiles(pendingJob, fileBuffer, kMaxVisibleFiles, count, total, stats)) return;
    pendingJob = 0;
    loaded = true;
    linear_progress::hide(progress);
    render(stats, total, count);
}

void screenRefreshCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_REFRESH) return;
    // This filter is page-local. Every entry follows the active source again,
    // making inspection of the inactive dataset a deliberate action.
    selectedDataset = historical_storage::activeDataset();
    updateDatasetTab();
    loaded = false;
    // A hidden screen's request can be superseded by the Usage query. Queue a
    // fresh one on activation so this page cannot remain permanently pending.
    pendingJob = 0;
    startLoad();
}

void datasetChangedCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    selectedDataset = lv_tabview_get_tab_act(datasetTabview) == 1
        ? historical_storage::Dataset::Demo : historical_storage::Dataset::Real;
    loaded = false;
    pendingJob = 0;
    startLoad();
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    screenObject = screen;
    ui_theme::styleScreen(screen, 4);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 4, 0);

    // Match the view-only Station / Access Point selector on the Wi-Fi page.
    // The tab pages are intentionally empty: the shared catalog below is
    // refreshed for whichever dataset tab is active.
    datasetTabview = lv_tabview_create(screen, LV_DIR_TOP, 36);
    lv_obj_set_size(datasetTabview, lv_pct(100), 36);
    lv_obj_set_style_bg_color(datasetTabview, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(datasetTabview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(datasetTabview, 0, 0);
    lv_obj_set_style_radius(datasetTabview, 0, 0);
    lv_obj_set_style_shadow_width(datasetTabview, 0, 0);
    lv_obj_set_style_pad_all(datasetTabview, 0, 0);
    lv_tabview_add_tab(datasetTabview, "Real");
    lv_tabview_add_tab(datasetTabview, "Demo");
    lv_obj_add_event_cb(datasetTabview, datasetChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    selectedDataset = historical_storage::activeDataset();
    updateDatasetTab();

    lv_obj_t* summary = lv_obj_create(screen);
    lv_obj_remove_style_all(summary);
    lv_obj_set_size(summary, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(summary, 6, 0);
    lv_obj_set_style_pad_row(summary, 2, 0);
    summaryLabel = addLine(summary, "--", ui_theme::text(), &lv_font_montserrat_14);
    detailLabel = addLine(summary, "--", ui_theme::mutedText(), &lv_font_montserrat_12);

    filesList = lv_obj_create(screen);
    lv_obj_remove_style_all(filesList);
    lv_obj_set_width(filesList, lv_pct(100));
    lv_obj_set_flex_grow(filesList, 1);
    lv_obj_set_flex_flow(filesList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(filesList, 4, 0);
    lv_obj_set_scrollbar_mode(filesList, LV_SCROLLBAR_MODE_AUTO);
    progress = linear_progress::create(screen);
    lv_obj_add_event_cb(screen, screenRefreshCb, LV_EVENT_REFRESH, nullptr);

    // Completion polling is UI-only; each page entry and filter change starts
    // a fresh bounded catalog request.
    lv_timer_create(completionCb, 40, nullptr);
    return screen;
}

} // namespace history_screen
