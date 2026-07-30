#include "wifi_screen.h"
#include "network/network_manager.h"
#include "../../theme/ui_theme.h"
#include "../../navigation/tabview_utils.h"
#include <Arduino.h>

namespace wifi_screen {
namespace {

constexpr uint32_t kPollIntervalMs = 200;
constexpr uint32_t kApPollIntervalMs = 1000;
constexpr int kMinApPasswordLen = 8;

using namespace network_manager;

// ---- UI References ----
lv_obj_t* modeTabview = nullptr;
lv_obj_t* clientPanel = nullptr;
lv_obj_t* apPanel = nullptr;

// New Prominent Connection UI
lv_obj_t* activeConnPanel = nullptr;
lv_obj_t* activeSsidLabel = nullptr;
lv_obj_t* activeStatusLabel = nullptr;
lv_obj_t* activeRssiLabel = nullptr;
lv_obj_t* activeIpLabel = nullptr;

lv_obj_t* scanLabel = nullptr;
lv_obj_t* scanBtn = nullptr;
lv_obj_t* scanBtnLabel = nullptr;
lv_obj_t* savedBtn = nullptr;
lv_obj_t* networkList = nullptr;
lv_timer_t* pollTimer = nullptr;

lv_obj_t* pwdOverlay = nullptr;
lv_obj_t* pwdTextarea = nullptr;
lv_obj_t* pwdErrorLabel = nullptr;
lv_obj_t* savedOverlay = nullptr;
lv_obj_t* savedList = nullptr;
lv_obj_t* forgetConfirm = nullptr;

char pendingSsid[33] = "";
char pendingForgetSsid[33] = "";
NetworkState lastState = NetworkState::Disconnected;
uint32_t lastScanGeneration = 0;

struct NetworkInfo {
    char ssid[33];
    bool secured;
};

struct SavedNetworkInfo {
    char ssid[33];
};

// ---- Access point mode ----
lv_obj_t* apSsidInput = nullptr;
lv_obj_t* apSecureSwitch = nullptr;
lv_obj_t* apPasswordInput = nullptr;
lv_obj_t* apPasswordStatus = nullptr;
lv_obj_t* apReplaceButton = nullptr;
lv_obj_t* apReplaceButtonLabel = nullptr;
lv_obj_t* apToggle = nullptr;
lv_obj_t* apInfoLabel = nullptr;
lv_obj_t* apCountLabel = nullptr;
lv_obj_t* apClientList = nullptr;
lv_obj_t* apKeyboard = nullptr;
lv_timer_t* apPollTimer = nullptr;
int lastApStationCount = -1;
bool hasScanned = false; // Track if we should say "Scan" or "Rescan"
bool apPasswordConfigured = false;
bool apReplacingPassword = false;


// ============================================================
// UI Updates
// ============================================================

bool isConnected() {
    NetworkState st = network_manager::getState();
    return (st == NetworkState::ConnectedStaLocal || st == NetworkState::ConnectedStaInternet);
}

void updateTabLabels() {
    if (!modeTabview) return;
    lv_tabview_rename_tab(modeTabview, 0, isConnected() ? LV_SYMBOL_OK " Station" : "Station");
    char buffer[128];
    if (network_manager::isApEnabled()) {
        snprintf(buffer, sizeof(buffer), "%s (%d Clients)", LV_SYMBOL_OK, lastApStationCount);
        lv_tabview_rename_tab(modeTabview, 1, buffer);
    } else {
        lv_tabview_rename_tab(modeTabview, 1, "Access Point");
    }
}

void updateListConnectionIcons() {
    if (!networkList) return;
    const char* connectedSsid = network_manager::getCurrentSsid();
    bool connected = isConnected();

    uint32_t cnt = lv_obj_get_child_cnt(networkList);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* btn = lv_obj_get_child(networkList, i);
        auto* info = (NetworkInfo*)lv_obj_get_user_data(btn);
        if (!info) continue;

        bool isThisNetwork = (connected && strcmp(info->ssid, connectedSsid) == 0);
        lv_obj_t* icon = lv_obj_get_child(btn, 0);
        if (icon) {
            lv_img_set_src(icon, isThisNetwork ? LV_SYMBOL_OK : LV_SYMBOL_WIFI);
        }
        if (isThisNetwork) lv_obj_add_state(btn, LV_STATE_CHECKED);
        else lv_obj_clear_state(btn, LV_STATE_CHECKED);
    }
}

void refreshConnectionLabel() {
    NetworkState st = network_manager::getState();
    ConnectionPhase phase = network_manager::getConnectionPhase();
    ConnectionFailure failure = network_manager::getConnectionFailure();
    RecoveryState recovery = network_manager::getRecoveryState();
    const char* ssid = network_manager::getCurrentSsid();

    if (st == NetworkState::Disconnected &&
        recovery == RecoveryState::Disabled &&
        (ssid[0] == '\0' || phase == ConnectionPhase::Idle)) {
        lv_obj_add_flag(activeConnPanel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(activeConnPanel, LV_OBJ_FLAG_HIDDEN);

    const bool genericRecovery =
        st == NetworkState::Disconnected &&
        (recovery == RecoveryState::Discovering ||
         recovery == RecoveryState::Waiting ||
         recovery == RecoveryState::Blocked);
    lv_label_set_text(activeSsidLabel,
                      genericRecovery || ssid[0] == '\0'
                          ? "Saved networks"
                          : ssid);
    const ScanState scan = network_manager::getScanState();
    if (st == NetworkState::Disconnected &&
        recovery == RecoveryState::Discovering) {
        lv_label_set_text(activeStatusLabel, "Looking for saved networks...");
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
        return;
    }
    if (st == NetworkState::Disconnected &&
        recovery == RecoveryState::Waiting) {
        const uint32_t seconds = network_manager::getReconnectSecondsRemaining();
        lv_label_set_text_fmt(activeStatusLabel,
                              failure == ConnectionFailure::NetworkNotFound
                                  ? "None nearby - checking in %lus"
                                  : "Trying saved networks in %lus",
                              static_cast<unsigned long>(seconds));
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
        return;
    }
    if (st == NetworkState::Disconnected &&
        recovery == RecoveryState::Blocked) {
        lv_label_set_text(activeStatusLabel, "Saved networks need attention");
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
        return;
    }
    if (st == NetworkState::Disconnected &&
        recovery == RecoveryState::FastRetry) {
        const uint32_t seconds = network_manager::getReconnectSecondsRemaining();
        if (failure == ConnectionFailure::LinkLost) {
            lv_label_set_text(activeStatusLabel, "Connection lost - reconnecting...");
        } else {
            lv_label_set_text_fmt(activeStatusLabel,
                                  failure == ConnectionFailure::NetworkNotFound
                                      ? "Not nearby - retry in %lus"
                                      : "Connection failed - retry in %lus",
                                  static_cast<unsigned long>(seconds));
        }
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
        return;
    }
    if (st == NetworkState::Disconnected &&
        (scan == ScanState::Starting || scan == ScanState::Running)) {
        lv_label_set_text(activeStatusLabel, "Retry paused for scan");
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
        return;
    }
    if (st == NetworkState::ConnectingSta ||
        phase == ConnectionPhase::LookingForNetwork) {
        lv_label_set_text(activeStatusLabel,
                          phase == ConnectionPhase::ObtainingIp
                              ? "Obtaining IP address..."
                              : "Looking for network...");
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
    } else if (st == NetworkState::ConnectedStaLocal) {
        lv_label_set_text(activeStatusLabel, LV_SYMBOL_WARNING " No Internet");
        lv_label_set_text_fmt(activeRssiLabel, "%d dBm", network_manager::getRssi());
        lv_label_set_text_fmt(activeIpLabel, "IP: %s", network_manager::getStaIpAddress());
    } else if (st == NetworkState::ConnectedStaInternet) {
        lv_label_set_text(activeStatusLabel, LV_SYMBOL_OK " Internet Connected");
        lv_label_set_text_fmt(activeRssiLabel, "%d dBm", network_manager::getRssi());
        lv_label_set_text_fmt(activeIpLabel, "IP: %s", network_manager::getStaIpAddress());
    } else if (phase == ConnectionPhase::RetryWaiting) {
        const uint32_t seconds = network_manager::getReconnectSecondsRemaining();
        const char* reason = "Connection failed";
        if (failure == ConnectionFailure::NetworkNotFound) reason = "Not nearby";
        else if (failure == ConnectionFailure::LinkLost) reason = "Connection lost";
        else if (failure == ConnectionFailure::TimedOut) reason = "Connection timed out";
        lv_label_set_text_fmt(activeStatusLabel, "%s - retry in %lus",
                              reason, static_cast<unsigned long>(seconds));
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
    } else if (phase == ConnectionPhase::ActionRequired) {
        lv_label_set_text(activeStatusLabel,
                          failure == ConnectionFailure::AuthenticationFailed
                              ? "Authentication failed"
                              : "Network security changed");
        lv_label_set_text(activeRssiLabel, "");
        lv_label_set_text(activeIpLabel, "");
    }
}

void setScanBusy(bool busy) {
    if (!scanBtn || !scanBtnLabel) return;
    if (busy) {
        lv_obj_add_state(scanBtn, LV_STATE_DISABLED);
        if (savedBtn) lv_obj_add_state(savedBtn, LV_STATE_DISABLED);
        lv_label_set_text(scanBtnLabel, "Scanning...");
    } else {
        lv_obj_clear_state(scanBtn, LV_STATE_DISABLED);
        if (savedBtn) lv_obj_clear_state(savedBtn, LV_STATE_DISABLED);
        lv_label_set_text(scanBtnLabel, hasScanned ? LV_SYMBOL_REFRESH " Rescan" : LV_SYMBOL_REFRESH " Scan");
    }
}

// ============================================================
// Prompts & Callbacks
// ============================================================

void closePasswordPrompt() {
    if (pwdOverlay) {
        if (pwdTextarea) lv_textarea_set_text(pwdTextarea, "");
        lv_obj_del(pwdOverlay);
        pwdOverlay = nullptr;
        pwdTextarea = nullptr;
        pwdErrorLabel = nullptr;
    }
    pendingSsid[0] = '\0';
}

void pwdConnectCb(lv_event_t*) {
    const char* password = lv_textarea_get_text(pwdTextarea);
    const size_t length = strlen(password);
    if (length < 8 || length > 63) {
        lv_label_set_text(pwdErrorLabel, "Use 8 to 63 characters.");
        return;
    }
    network_manager::connectTo(pendingSsid, password);
    closePasswordPrompt();
    refreshConnectionLabel();
}

void pwdCancelCb(lv_event_t*) { closePasswordPrompt(); }

void pwdKeyboardEventCb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) pwdConnectCb(e);
    else if (code == LV_EVENT_CANCEL) pwdCancelCb(e);
}

void showPasswordPrompt(const char* ssid) {
    strncpy(pendingSsid, ssid, sizeof(pendingSsid) - 1);
    pendingSsid[sizeof(pendingSsid) - 1] = '\0';

    pwdOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pwdOverlay);
    lv_obj_set_size(pwdOverlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(pwdOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pwdOverlay, LV_OPA_70, 0); // Darkened slightly for focus
    lv_obj_set_flex_flow(pwdOverlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pwdOverlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* panel = lv_obj_create(pwdOverlay);
    lv_obj_set_size(panel, lv_pct(85), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 16, 0); // Increased padding
    lv_obj_set_style_pad_row(panel, 10, 0); // Gap between title/input/buttons

    char title[48];
    snprintf(title, sizeof(title), "Password for %s", ssid);
    lv_obj_t* titleLabel = lv_label_create(panel);
    lv_label_set_text(titleLabel, title);

    pwdTextarea = lv_textarea_create(panel);
    lv_textarea_set_one_line(pwdTextarea, true);
    lv_textarea_set_password_mode(pwdTextarea, true); // Mask password visually
    lv_textarea_set_max_length(pwdTextarea, 63);
    lv_textarea_set_placeholder_text(pwdTextarea, "Enter Password");
    lv_obj_set_width(pwdTextarea, lv_pct(100));

    pwdErrorLabel = lv_label_create(panel);
    lv_obj_set_style_text_color(pwdErrorLabel, lv_palette_main(LV_PALETTE_RED), 0);
    lv_label_set_text(pwdErrorLabel, "");

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_remove_style_all(btnRow);
    lv_obj_set_size(btnRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btnRow, 10, 0); // Fix buttons touching

    lv_obj_t* cancelBtn = lv_btn_create(btnRow);
    lv_obj_set_flex_grow(cancelBtn, 1);
    lv_obj_add_event_cb(cancelBtn, pwdCancelCb, LV_EVENT_CLICKED, nullptr);
    lv_label_set_text(lv_label_create(cancelBtn), "Cancel");

    lv_obj_t* connectBtn = lv_btn_create(btnRow);
    lv_obj_set_flex_grow(connectBtn, 1);
    lv_obj_add_event_cb(connectBtn, pwdConnectCb, LV_EVENT_CLICKED, nullptr);
    lv_label_set_text(lv_label_create(connectBtn), "Connect");

    lv_obj_t* keyboard = lv_keyboard_create(pwdOverlay);
    lv_keyboard_set_textarea(keyboard, pwdTextarea);
    lv_obj_add_event_cb(keyboard, pwdKeyboardEventCb, LV_EVENT_ALL, nullptr);
}

void closeSavedOverlay() {
    if (forgetConfirm) {
        lv_obj_del(forgetConfirm);
        forgetConfirm = nullptr;
    }
    if (savedOverlay) {
        lv_obj_del(savedOverlay);
        savedOverlay = nullptr;
        savedList = nullptr;
    }
}

void savedInfoDeleteCb(lv_event_t* event) {
    void* info = lv_obj_get_user_data(lv_event_get_target(event));
    if (info) free(info);
}

void rebuildSavedList();

void savedConnectCb(lv_event_t* event) {
    auto* info = static_cast<SavedNetworkInfo*>(lv_event_get_user_data(event));
    if (!info) return;
    if (network_manager::connectSavedNetwork(info->ssid)) {
        closeSavedOverlay();
        refreshConnectionLabel();
    }
}

void forgetConfirmCb(lv_event_t* event) {
    const char* action = lv_msgbox_get_active_btn_text(lv_event_get_current_target(event));
    if (!action) return;
    if (strcmp(action, "Forget") == 0) {
        network_manager::forgetSavedNetwork(pendingForgetSsid);
        rebuildSavedList();
        refreshConnectionLabel();
        updateListConnectionIcons();
        updateTabLabels();
    }
    lv_obj_t* messageBox = lv_event_get_current_target(event);
    forgetConfirm = nullptr;
    lv_msgbox_close_async(messageBox);
}

void savedForgetCb(lv_event_t* event) {
    auto* info = static_cast<SavedNetworkInfo*>(lv_event_get_user_data(event));
    if (!info || forgetConfirm) return;
    strncpy(pendingForgetSsid, info->ssid, sizeof(pendingForgetSsid) - 1);
    pendingForgetSsid[sizeof(pendingForgetSsid) - 1] = '\0';
    static const char* buttons[] = {"Forget", "Cancel", ""};
    forgetConfirm = lv_msgbox_create(lv_layer_top(), "Forget network?",
                                     pendingForgetSsid, buttons, false);
    lv_obj_set_width(forgetConfirm, lv_pct(88));
    lv_obj_add_event_cb(forgetConfirm, forgetConfirmCb,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_center(forgetConfirm);
}

void rebuildSavedList() {
    if (!savedList) return;
    lv_obj_clean(savedList);
    const int count = network_manager::getSavedNetworkCount();
    if (count <= 0) {
        lv_label_set_text(lv_label_create(savedList), "No saved networks");
        return;
    }

    for (int i = 0; i < count; ++i) {
        char ssid[33];
        if (!network_manager::getSavedNetwork(i, ssid, sizeof(ssid))) continue;

        auto* info = static_cast<SavedNetworkInfo*>(malloc(sizeof(SavedNetworkInfo)));
        if (!info) continue;
        strncpy(info->ssid, ssid, sizeof(info->ssid) - 1);
        info->ssid[sizeof(info->ssid) - 1] = '\0';

        lv_obj_t* row = lv_obj_create(savedList);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
            LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 4, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, ui_theme::border(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(
            row, ui_theme::surfaceAlt(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, info);
        lv_obj_add_event_cb(row, savedInfoDeleteCb, LV_EVENT_DELETE, nullptr);
        lv_obj_add_event_cb(row, savedConnectCb, LV_EVENT_CLICKED, info);

        lv_obj_t* wifiIcon = lv_label_create(row);
        lv_obj_set_width(wifiIcon, 28);
        lv_obj_set_style_text_align(wifiIcon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(wifiIcon, ui_theme::mutedText(), 0);
        lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, ssid);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, 0);
        lv_obj_set_flex_grow(label, 1);

        lv_obj_t* forgetButton = lv_btn_create(row);
        lv_obj_remove_style_all(forgetButton);
        lv_obj_set_size(forgetButton, 42, 40);
        lv_obj_set_ext_click_area(forgetButton, 2);
        lv_obj_add_event_cb(forgetButton, savedForgetCb,
                            LV_EVENT_CLICKED, info);
        lv_obj_t* forgetLabel = lv_label_create(forgetButton);
        lv_label_set_text(forgetLabel, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(
            forgetLabel, ui_theme::mutedText(), 0);
        lv_obj_center(forgetLabel);
    }
}

void savedCloseCb(lv_event_t*) { closeSavedOverlay(); }

void showSavedOverlay(lv_event_t*) {
    if (savedOverlay) return;
    savedOverlay = lv_obj_create(lv_layer_top());
    ui_theme::styleScreen(savedOverlay, 5);
    lv_obj_set_flex_flow(savedOverlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(savedOverlay, 6, 0);

    lv_obj_t* titleRow = lv_obj_create(savedOverlay);
    lv_obj_remove_style_all(titleRow);
    lv_obj_set_size(titleRow, lv_pct(100), 42);
    lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* backButton = lv_btn_create(titleRow);
    lv_obj_remove_style_all(backButton);
    lv_obj_set_size(backButton, 38, 40);
    lv_obj_set_ext_click_area(backButton, 6);
    lv_obj_add_event_cb(
        backButton, savedCloseCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLabel = lv_label_create(backButton);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLabel, ui_theme::accent(), 0);
    lv_obj_center(backLabel);
    lv_obj_t* title = lv_label_create(titleRow);
    lv_label_set_text(title, "Saved networks");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

    savedList = lv_list_create(savedOverlay);
    ui_theme::styleCard(savedList, 2);
    lv_obj_set_width(savedList, lv_pct(100));
    lv_obj_set_height(savedList, 0);
    lv_obj_set_flex_grow(savedList, 1);
    lv_obj_set_flex_flow(savedList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(savedList, 0, 0);
    rebuildSavedList();

    lv_obj_t* actionRow = lv_obj_create(savedOverlay);
    lv_obj_remove_style_all(actionRow);
    lv_obj_set_size(actionRow, lv_pct(100), 40);
    lv_obj_set_flex_flow(actionRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actionRow, 5, 0);

    lv_obj_t* cancelButton = lv_btn_create(actionRow);
    lv_obj_set_height(cancelButton, 40);
    lv_obj_set_flex_grow(cancelButton, 1);
    lv_obj_set_style_bg_color(cancelButton, ui_theme::surfaceAlt(), 0);
    lv_obj_set_style_bg_opa(cancelButton, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancelButton, ui_theme::border(), 0);
    lv_obj_set_style_border_width(cancelButton, 1, 0);
    lv_obj_set_style_radius(cancelButton, 6, 0);
    lv_obj_set_style_text_color(cancelButton, ui_theme::text(), 0);
    lv_obj_add_event_cb(
        cancelButton, savedCloseCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancelLabel = lv_label_create(cancelButton);
    lv_label_set_text(cancelLabel, "Cancel");
    lv_obj_center(cancelLabel);

    lv_obj_t* actionSpacer = lv_obj_create(actionRow);
    lv_obj_remove_style_all(actionSpacer);
    lv_obj_set_height(actionSpacer, 40);
    lv_obj_set_flex_grow(actionSpacer, 1);
}

void rowDeleteCb(lv_event_t* e) {
    void* info = lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    if (info) free(info);
}

void rowClickCb(lv_event_t* e) {
    auto* info = (NetworkInfo*)lv_event_get_user_data(e);
    if (!info) return;

    if (info->secured) {
        bool saved = false;
        const int count = network_manager::getSavedNetworkCount();
        for (int i = 0; i < count; ++i) {
            char savedSsid[33];
            if (network_manager::getSavedNetwork(
                    i, savedSsid, sizeof(savedSsid)) &&
                strcmp(savedSsid, info->ssid) == 0) {
                saved = true;
                break;
            }
        }
        if (saved) network_manager::connectSavedNetwork(info->ssid);
        else showPasswordPrompt(info->ssid);
    } else {
        network_manager::connectTo(info->ssid);
    }
    refreshConnectionLabel();
}

void rebuildListFromScan() {
    lv_obj_clean(networkList);
    setScanBusy(false);
    hasScanned = true;

    int n = network_manager::getScanResultCount();
    if (n <= 0) {
        lv_list_add_text(networkList, "No networks found");
        lv_label_set_text(scanLabel, "No networks found.");
        return;
    }

    for (int i = 0; i < n; i++) {
        char ssid[33];
        bool secured;
        int rssi;
        network_manager::getScanResult(i, ssid, sizeof(ssid), secured, rssi);

        bool connected = (isConnected() && strcmp(network_manager::getCurrentSsid(), ssid) == 0);
        lv_obj_t* btn = lv_list_add_btn(networkList, connected ? LV_SYMBOL_OK : LV_SYMBOL_WIFI, ssid);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);

        auto* info = (NetworkInfo*)malloc(sizeof(NetworkInfo));
        strncpy(info->ssid, ssid, sizeof(info->ssid));
        info->secured = secured;

        lv_obj_set_user_data(btn, info);
        lv_obj_add_event_cb(btn, rowClickCb, LV_EVENT_CLICKED, info);
        lv_obj_add_event_cb(btn, rowDeleteCb, LV_EVENT_DELETE, nullptr);
        if (connected) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    char status[48];
    snprintf(status, sizeof(status), "Found %d network%s.", n, n == 1 ? "" : "s");
    lv_label_set_text(scanLabel, status);
}

void scanEventCb(lv_event_t*) {
    if (!network_manager::scanNetworks()) {
        lv_label_set_text(scanLabel, "Could not start scan.");
        setScanBusy(false);
        return;
    }
    lv_obj_clean(networkList);
    lv_list_add_text(networkList, "Scanning...");
    setScanBusy(true);
    lv_label_set_text(scanLabel, "Scanning for networks...");
}

void pollCb(lv_timer_t* timer) {
    if (timer && timer->user_data && !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) return;
    NetworkState currentState = network_manager::getState();
    const ScanState currentScanState = network_manager::getScanState();
    const uint32_t scanGeneration = network_manager::getScanGeneration();

    setScanBusy(currentScanState == ScanState::Starting ||
                currentScanState == ScanState::Running);
    if (scanGeneration != lastScanGeneration) {
        lastScanGeneration = scanGeneration;
        if (currentScanState == ScanState::Succeeded) {
            rebuildListFromScan();
        } else if (currentScanState == ScanState::Failed) {
            lv_obj_clean(networkList);
            lv_list_add_text(networkList, "Scan failed");
            lv_label_set_text(scanLabel, "Scan failed. Try again.");
            setScanBusy(false);
        }
    }

    if (currentState != lastState) {
        lastState = currentState;
        updateListConnectionIcons();
        updateTabLabels();
    }
    refreshConnectionLabel();
}

// ============================================================
// AP Event Handlers
// ============================================================

void apRefreshClientList() {
    int n = network_manager::getApClientCount();
    if (n == lastApStationCount) return;
    lastApStationCount = n;
    updateTabLabels();

    char countBuf[32];
    snprintf(countBuf, sizeof(countBuf), "%d device%s connected", n, n == 1 ? "" : "s");
    lv_label_set_text(apCountLabel, countBuf);

    lv_obj_clean(apClientList);
    if (n == 0) {
        lv_list_add_text(apClientList, "No devices connected");
        return;
    }

    for (int i = 0; i < n; i++) {
        char macStr[18];
        if (network_manager::getApClientMac(i, macStr, sizeof(macStr))) {
            lv_list_add_text(apClientList, macStr);
        }
    }
}

void apPollCb(lv_timer_t* timer) {
    if (timer && timer->user_data && !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) return;
    if (!network_manager::isApEnabled()) return;
    apRefreshClientList();
}

void clearApPasswordEditor() {
    if (apPasswordInput) lv_textarea_set_text(apPasswordInput, "");
    if (apKeyboard) {
        lv_keyboard_set_textarea(apKeyboard, nullptr);
        lv_obj_add_flag(apKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void apUpdatePasswordEditor() {
    if (!apPasswordInput || !apPasswordStatus || !apReplaceButton) return;
    const bool secure =
        lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED);
    const bool running = network_manager::isApEnabled();

    if (!secure) {
        apReplacingPassword = false;
        clearApPasswordEditor();
        lv_label_set_text(apPasswordStatus, "Open access point (no password)");
        lv_obj_add_flag(apReplaceButton, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(apPasswordInput, LV_STATE_DISABLED);
        return;
    }

    if (!apPasswordConfigured) apReplacingPassword = true;
    if (apReplacingPassword) {
        lv_label_set_text(apPasswordStatus, "Enter a new password");
        if (apPasswordConfigured) {
            lv_obj_clear_flag(apReplaceButton, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(apReplaceButtonLabel, "Cancel");
        } else {
            lv_obj_add_flag(apReplaceButton, LV_OBJ_FLAG_HIDDEN);
        }
        if (running) lv_obj_add_state(apPasswordInput, LV_STATE_DISABLED);
        else lv_obj_clear_state(apPasswordInput, LV_STATE_DISABLED);
    } else {
        clearApPasswordEditor();
        lv_label_set_text(apPasswordStatus, "Saved password (not shown)");
        lv_obj_clear_flag(apReplaceButton, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(apReplaceButtonLabel, "Replace");
        lv_obj_add_state(apPasswordInput, LV_STATE_DISABLED);
    }

    if (running) lv_obj_add_state(apReplaceButton, LV_STATE_DISABLED);
    else lv_obj_clear_state(apReplaceButton, LV_STATE_DISABLED);
}

void apUpdateToggleEnabled() {
    if (!apToggle) return;
    if (network_manager::isApEnabled()) {
        lv_obj_clear_state(apToggle, LV_STATE_DISABLED);
        return;
    }
    const char* ssid = lv_textarea_get_text(apSsidInput);
    bool secure = lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED);
    const char* password = lv_textarea_get_text(apPasswordInput);
    const size_t passwordLength = strlen(password);
    const bool validPassword =
        !secure ||
        (!apReplacingPassword && apPasswordConfigured) ||
        (apReplacingPassword && passwordLength >= kMinApPasswordLen &&
         passwordLength <= 63);
    bool valid = strlen(ssid) > 0 && validPassword;

    if (valid) lv_obj_clear_state(apToggle, LV_STATE_DISABLED);
    else lv_obj_add_state(apToggle, LV_STATE_DISABLED);
}

void apSecureSwitchEventCb(lv_event_t*) {
    if (!lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED)) {
        apReplacingPassword = false;
        clearApPasswordEditor();
    } else if (!apPasswordConfigured) {
        apReplacingPassword = true;
    }
    apUpdatePasswordEditor();
    apUpdateToggleEnabled();
}

void apFormFieldChangedCb(lv_event_t*) { apUpdateToggleEnabled(); }

void apReplacePasswordCb(lv_event_t*) {
    if (apReplacingPassword && apPasswordConfigured) {
        apReplacingPassword = false;
        clearApPasswordEditor();
    } else {
        apReplacingPassword = true;
        clearApPasswordEditor();
    }
    apUpdatePasswordEditor();
    apUpdateToggleEnabled();
}

void apInputFocusCb(lv_event_t* e) {
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    lv_keyboard_set_textarea(apKeyboard, ta);
    lv_obj_clear_flag(apKeyboard, LV_OBJ_FLAG_HIDDEN);
}

void apInputDefocusCb(lv_event_t*) {
    lv_obj_add_flag(apKeyboard, LV_OBJ_FLAG_HIDDEN);
}

void apKeyboardEventCb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(apKeyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* ta = lv_keyboard_get_textarea(apKeyboard);
        if (ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    }
}

void apToggleEventCb(lv_event_t*) {
    bool wantOn = lv_obj_has_state(apToggle, LV_STATE_CHECKED);

    if (wantOn) {
        const char* ssid = lv_textarea_get_text(apSsidInput);
        bool secure = lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED);
        const char* password = lv_textarea_get_text(apPasswordInput);
        const ApPasswordAction passwordAction =
            !secure ? ApPasswordAction::Remove
                    : (apReplacingPassword ? ApPasswordAction::Replace
                                           : ApPasswordAction::Keep);
        const char* replacement =
            passwordAction == ApPasswordAction::Replace ? password : nullptr;
        const ApStartResult result =
            network_manager::startAp(ssid, secure, passwordAction, replacement);
        clearApPasswordEditor();
        if (result != ApStartResult::Success) {
            lv_obj_clear_state(apToggle, LV_STATE_CHECKED);
            lv_label_set_text(
                apInfoLabel,
                result == ApStartResult::InvalidSettings
                    ? "Invalid access point settings."
                    : "Could not start access point.");
            apUpdatePasswordEditor();
            apUpdateToggleEnabled();
            return;
        }
        apPasswordConfigured = secure;
        apReplacingPassword = false;
        lastApStationCount = -1;
        lv_obj_add_state(apSsidInput, LV_STATE_DISABLED);
        lv_obj_add_state(apSecureSwitch, LV_STATE_DISABLED);
        apUpdatePasswordEditor();
        lv_label_set_text_fmt(apInfoLabel, "IP: %s", network_manager::getApIpAddress());
        apRefreshClientList();
    } else {
        network_manager::stopAp();
        lastApStationCount = -1;
        lv_obj_clear_state(apSsidInput, LV_STATE_DISABLED);
        lv_obj_clear_state(apSecureSwitch, LV_STATE_DISABLED);
        clearApPasswordEditor();
        apReplacingPassword = false;
        apUpdatePasswordEditor();
        lv_label_set_text(apInfoLabel, "Access point stopped.");
        lv_obj_clean(apClientList);
        lv_list_add_text(apClientList, "Access point not running");
        lv_label_set_text(apCountLabel, "0 devices connected");
    }
    apUpdateToggleEnabled();
    updateTabLabels();
}

// ============================================================
// Core Initialization & Tab Switching
// ============================================================

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* scr = lv_obj_create(parent);
    ui_theme::styleScreen(scr, 4);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 4, 0);

    // ---- Station / AP sub-tabs ----
    modeTabview = lv_tabview_create(scr, LV_DIR_TOP, 36);
    tabview_utils::disableSwipe(modeTabview);
    lv_obj_set_size(modeTabview, lv_pct(100), 0);
    lv_obj_set_flex_grow(modeTabview, 1);
    lv_obj_set_style_bg_color(modeTabview, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(modeTabview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modeTabview, 0, 0);
    lv_obj_set_style_radius(modeTabview, 0, 0);
    lv_obj_set_style_shadow_width(modeTabview, 0, 0);
    lv_obj_set_style_pad_all(modeTabview, 0, 0);

    // ---- Client panel ----
    clientPanel = lv_tabview_add_tab(modeTabview, "Station");
    lv_obj_set_style_bg_color(clientPanel, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(clientPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(clientPanel, 0, 0);
    lv_obj_set_style_radius(clientPanel, 0, 0);
    lv_obj_set_style_shadow_width(clientPanel, 0, 0);
    lv_obj_set_style_pad_all(clientPanel, 0, 0);
    lv_obj_set_flex_flow(clientPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(clientPanel, 4, 0);

    // 1. Prominent Active Connection Panel (Hidden by default)
    activeConnPanel = lv_obj_create(clientPanel);
    ui_theme::styleCard(activeConnPanel, 5);
    lv_obj_set_size(activeConnPanel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(activeConnPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(activeConnPanel, 2, 0);
    lv_obj_add_flag(activeConnPanel, LV_OBJ_FLAG_HIDDEN);

    activeSsidLabel = lv_label_create(activeConnPanel);
    lv_obj_set_style_text_font(activeSsidLabel, &lv_font_montserrat_18, 0); // Larger font if available
    lv_obj_set_width(activeSsidLabel, lv_pct(100));
    lv_label_set_long_mode(activeSsidLabel, LV_LABEL_LONG_DOT);

    activeStatusLabel = lv_label_create(activeConnPanel);
    lv_obj_set_width(activeStatusLabel, lv_pct(100));
    lv_label_set_long_mode(activeStatusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_t* activeDetailsRow = lv_obj_create(activeConnPanel);
    lv_obj_remove_style_all(activeDetailsRow);
    lv_obj_set_size(activeDetailsRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(activeDetailsRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(activeDetailsRow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    activeIpLabel = lv_label_create(activeDetailsRow);
    activeRssiLabel = lv_label_create(activeDetailsRow);
    lv_obj_set_style_text_align(activeRssiLabel, LV_TEXT_ALIGN_RIGHT, 0);

    // 2. Scan status
    scanLabel = lv_label_create(clientPanel);
    lv_obj_set_width(scanLabel, lv_pct(100));
    lv_label_set_text(scanLabel, "Tap Scan to find networks.");

    // 3. Network List
    networkList = lv_list_create(clientPanel);
    ui_theme::styleCard(networkList, 2);
    lv_obj_set_width(networkList, lv_pct(100));
    lv_obj_set_height(networkList, 0);
    lv_obj_set_flex_grow(networkList, 1);
    // styleCard intentionally removes inherited widget styling, so restore
    // the list layout explicitly and let each row span the available width.
    lv_obj_set_flex_flow(networkList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(networkList, 0, 0);
    lv_list_add_text(networkList, "No networks yet");

    // 4. Station actions
    lv_obj_t* stationActions = lv_obj_create(clientPanel);
    lv_obj_remove_style_all(stationActions);
    lv_obj_set_size(stationActions, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stationActions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stationActions, 4, 0);

    scanBtn = lv_btn_create(stationActions);
    lv_obj_set_flex_grow(scanBtn, 1);
    lv_obj_add_event_cb(scanBtn, scanEventCb, LV_EVENT_CLICKED, nullptr);
    ui_theme::stylePrimaryButton(scanBtn);
    scanBtnLabel = lv_label_create(scanBtn);
    lv_label_set_text(scanBtnLabel, LV_SYMBOL_REFRESH " Scan");
    lv_obj_center(scanBtnLabel);

    savedBtn = lv_btn_create(stationActions);
    lv_obj_set_flex_grow(savedBtn, 1);
    lv_obj_add_event_cb(savedBtn, showSavedOverlay, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* savedLabel = lv_label_create(savedBtn);
    lv_label_set_text(savedLabel, LV_SYMBOL_LIST " Saved");
    lv_obj_center(savedLabel);


    // ---- Access point panel ----
    apPanel = lv_tabview_add_tab(modeTabview, "Access Point");
    lv_obj_set_style_bg_color(apPanel, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(apPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(apPanel, 0, 0);
    lv_obj_set_style_radius(apPanel, 0, 0);
    lv_obj_set_style_shadow_width(apPanel, 0, 0);
    lv_obj_set_style_pad_all(apPanel, 0, 0);
    lv_obj_set_flex_flow(apPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(apPanel, 4, 0);

    apSsidInput = lv_textarea_create(apPanel);
    lv_textarea_set_one_line(apSsidInput, true);
    lv_textarea_set_max_length(apSsidInput, 32);
    lv_textarea_set_placeholder_text(apSsidInput, "Network Name (SSID)");
    lv_obj_set_width(apSsidInput, lv_pct(100));
    lv_obj_add_event_cb(apSsidInput, apInputFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(apSsidInput, apInputDefocusCb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(apSsidInput, apFormFieldChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* pwdRow = lv_obj_create(apPanel);
    lv_obj_remove_style_all(pwdRow);
    lv_obj_set_size(pwdRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pwdRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pwdRow, 12, 0); // Fix switch and input touching
    lv_obj_set_flex_align(pwdRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    apSecureSwitch = lv_switch_create(pwdRow);
    lv_obj_add_state(apSecureSwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(apSecureSwitch, apSecureSwitchEventCb, LV_EVENT_VALUE_CHANGED, nullptr);

    apPasswordStatus = lv_label_create(pwdRow);
    lv_obj_set_flex_grow(apPasswordStatus, 1);
    lv_label_set_long_mode(apPasswordStatus, LV_LABEL_LONG_DOT);

    apReplaceButton = lv_btn_create(pwdRow);
    lv_obj_add_event_cb(apReplaceButton, apReplacePasswordCb,
                        LV_EVENT_CLICKED, nullptr);
    apReplaceButtonLabel = lv_label_create(apReplaceButton);
    lv_label_set_text(apReplaceButtonLabel, "Replace");
    lv_obj_center(apReplaceButtonLabel);

    apPasswordInput = lv_textarea_create(apPanel);
    lv_textarea_set_one_line(apPasswordInput, true);
    lv_textarea_set_password_mode(apPasswordInput, true);
    lv_textarea_set_max_length(apPasswordInput, 63);
    lv_textarea_set_placeholder_text(apPasswordInput, "Access Point Password");
    lv_obj_set_width(apPasswordInput, lv_pct(100));
    lv_obj_add_event_cb(apPasswordInput, apInputFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(apPasswordInput, apInputDefocusCb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(apPasswordInput, apFormFieldChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* apToggleRow = lv_obj_create(apPanel);
    lv_obj_remove_style_all(apToggleRow);
    lv_obj_set_size(apToggleRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(apToggleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(apToggleRow, 12, 0); // Fix toggle touching text
    lv_obj_set_flex_align(apToggleRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    apToggle = lv_switch_create(apToggleRow);
    lv_obj_add_event_cb(apToggle, apToggleEventCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_label_set_text(lv_label_create(apToggleRow), "Enable Access Point");

    apInfoLabel = lv_label_create(apPanel);
    lv_obj_set_width(apInfoLabel, lv_pct(100));
    lv_label_set_text(apInfoLabel, "Access point stopped.");

    apCountLabel = lv_label_create(apPanel);
    lv_label_set_text(apCountLabel, "0 devices connected");

    apClientList = lv_list_create(apPanel);
    ui_theme::styleCard(apClientList, 2);
    lv_obj_set_width(apClientList, lv_pct(100));
    lv_obj_set_height(apClientList, 0);
    lv_obj_set_flex_grow(apClientList, 1);
    lv_obj_set_flex_flow(apClientList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(apClientList, 0, 0);

    apKeyboard = lv_keyboard_create(scr);
    lv_obj_add_flag(apKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(apKeyboard, apKeyboardEventCb, LV_EVENT_ALL, nullptr);

    // Initial persistence sync
    char savedSsid[33];
    bool savedSecure;
    network_manager::getSavedApSettings(
        savedSsid, sizeof(savedSsid), savedSecure, apPasswordConfigured);
    if (savedSsid[0] != '\0') lv_textarea_set_text(apSsidInput, savedSsid);
    if (savedSecure) lv_obj_add_state(apSecureSwitch, LV_STATE_CHECKED);
    else lv_obj_clear_state(apSecureSwitch, LV_STATE_CHECKED);

    apSecureSwitchEventCb(nullptr);
    if (network_manager::isApEnabled()) {
        lv_obj_add_state(apToggle, LV_STATE_CHECKED);
        lv_obj_add_state(apSsidInput, LV_STATE_DISABLED);
        lv_obj_add_state(apSecureSwitch, LV_STATE_DISABLED);
        apUpdatePasswordEditor();
        lv_label_set_text_fmt(apInfoLabel, "IP: %s", network_manager::getApIpAddress());
        lastApStationCount = -1;
        apRefreshClientList();
    }
    lv_tabview_set_act(modeTabview, 0, LV_ANIM_OFF);
    updateTabLabels();

    lastState = network_manager::getState();
    lastScanGeneration = network_manager::getScanGeneration();
    if (network_manager::getScanState() == ScanState::Succeeded &&
        lastScanGeneration > 0) {
        rebuildListFromScan();
    }
    refreshConnectionLabel();

    pollTimer = lv_timer_create(pollCb, kPollIntervalMs, scr);
    apPollTimer = lv_timer_create(apPollCb, kApPollIntervalMs, scr);

    return scr;
}

} // namespace wifi_screen
