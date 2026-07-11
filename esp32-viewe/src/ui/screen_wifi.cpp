#include "screen_wifi.h"
#include "screen_manager.h"
#include "network/network_manager.h"
#include <Arduino.h>

namespace screen_wifi {
namespace {

constexpr uint32_t kPollIntervalMs = 200;
constexpr uint32_t kApPollIntervalMs = 1000;
constexpr int kMinApPasswordLen = 8;

using namespace network_manager;

// ---- UI References ----
lv_obj_t* clientTabBtn = nullptr;
lv_obj_t* apTabBtn = nullptr;
lv_obj_t* clientTabLabel = nullptr;
lv_obj_t* apTabLabel = nullptr;
lv_obj_t* clientPanel = nullptr;
lv_obj_t* apPanel = nullptr;

// New Prominent Connection UI
lv_obj_t* activeConnPanel = nullptr;
lv_obj_t* activeSsidLabel = nullptr;
lv_obj_t* activeStatusLabel = nullptr;
lv_obj_t* activeRssiLabel = nullptr;

lv_obj_t* scanLabel = nullptr;
lv_obj_t* scanBtn = nullptr;
lv_obj_t* scanBtnLabel = nullptr;
lv_obj_t* networkList = nullptr;
lv_timer_t* pollTimer = nullptr;

lv_obj_t* pwdOverlay = nullptr;
lv_obj_t* pwdTextarea = nullptr;

char pendingSsid[33] = "";
NetworkState lastState = NetworkState::Disconnected;

struct NetworkInfo {
    char ssid[33];
    bool secured;
};

// ---- Access point mode ----
lv_obj_t* apSsidInput = nullptr;
lv_obj_t* apSecureSwitch = nullptr;
lv_obj_t* apPasswordInput = nullptr;
lv_obj_t* apToggle = nullptr;
lv_obj_t* apInfoLabel = nullptr;
lv_obj_t* apCountLabel = nullptr;
lv_obj_t* apClientList = nullptr;
lv_obj_t* apKeyboard = nullptr;
lv_timer_t* apPollTimer = nullptr;
int lastApStationCount = -1;
bool hasScanned = false; // Track if we should say "Scan" or "Rescan"

// ============================================================
// UI Updates
// ============================================================

bool isConnected() {
    NetworkState st = network_manager::getState();
    return (st == NetworkState::ConnectedStaLocal || st == NetworkState::ConnectedStaInternet);
}

void updateTabLabels() {
    if (clientTabLabel) {
        lv_label_set_text(clientTabLabel, isConnected() ? LV_SYMBOL_OK " Station" : "Station");
    }
    char buffer[128];
    if (network_manager::isApEnabled()) {
        snprintf(buffer, sizeof(buffer), "%s (%d Clients)", LV_SYMBOL_OK, lastApStationCount);
        lv_label_set_text(apTabLabel, buffer);
    } else {
        lv_label_set_text(apTabLabel, "Access Point");
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

    // Manage prominent active connection panel visibility
    if (st == NetworkState::Disconnected || st == NetworkState::Scanning) {
        lv_obj_add_flag(activeConnPanel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(activeConnPanel, LV_OBJ_FLAG_HIDDEN);

    if (st == NetworkState::ConnectingSta) {
        lv_label_set_text_fmt(activeSsidLabel, "Connecting to %s...", network_manager::getCurrentSsid());
        lv_label_set_text(activeStatusLabel, "Authenticating...");
        lv_label_set_text(activeRssiLabel, "");
    } else if (st == NetworkState::ConnectedStaLocal) {
        lv_label_set_text(activeSsidLabel, network_manager::getCurrentSsid());
        lv_label_set_text(activeStatusLabel, LV_SYMBOL_WARNING " Local Network Only");
        lv_label_set_text_fmt(activeRssiLabel, "Signal: %d dBm", network_manager::getRssi());
    } else if (st == NetworkState::ConnectedStaInternet) {
        lv_label_set_text(activeSsidLabel, network_manager::getCurrentSsid());
        lv_label_set_text(activeStatusLabel, LV_SYMBOL_OK " Internet Connected");
        lv_label_set_text_fmt(activeRssiLabel, "Signal: %d dBm", network_manager::getRssi());
    }
}

void setScanBusy(bool busy) {
    if (!scanBtn || !scanBtnLabel) return;
    if (busy) {
        lv_obj_add_state(scanBtn, LV_STATE_DISABLED);
        lv_label_set_text(scanBtnLabel, "Scanning...");
    } else {
        lv_obj_clear_state(scanBtn, LV_STATE_DISABLED);
        lv_label_set_text(scanBtnLabel, hasScanned ? LV_SYMBOL_REFRESH " Rescan" : LV_SYMBOL_REFRESH " Scan");
    }
}

// ============================================================
// Prompts & Callbacks
// ============================================================

void closePasswordPrompt() {
    if (pwdOverlay) {
        lv_obj_del(pwdOverlay);
        pwdOverlay = nullptr;
        pwdTextarea = nullptr;
    }
}

void pwdConnectCb(lv_event_t*) {
    const char* password = lv_textarea_get_text(pwdTextarea);
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
    lv_textarea_set_placeholder_text(pwdTextarea, "Enter Password");
    lv_obj_set_width(pwdTextarea, lv_pct(100));

    char savedPass[64];
    if (network_manager::getSavedPassword(ssid, savedPass, sizeof(savedPass))) {
        lv_textarea_set_text(pwdTextarea, savedPass);
    }

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

void rowDeleteCb(lv_event_t* e) {
    void* info = lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    if (info) free(info);
}

void rowClickCb(lv_event_t* e) {
    auto* info = (NetworkInfo*)lv_event_get_user_data(e);
    if (!info) return;

    if (info->secured) showPasswordPrompt(info->ssid);
    else {
        network_manager::connectTo(info->ssid);
        refreshConnectionLabel();
    }
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
    lv_obj_clean(networkList);
    lv_list_add_text(networkList, "Scanning...");
    setScanBusy(true);
    lv_label_set_text(scanLabel, "Scanning for WiFi networks...");
    network_manager::scanNetworks();
}

void pollCb(lv_timer_t*) {
    NetworkState currentState = network_manager::getState();

    if (currentState != lastState) {
        if (lastState == NetworkState::Scanning) {
            rebuildListFromScan();
        }
        lastState = currentState;
        refreshConnectionLabel();
        updateListConnectionIcons();
        updateTabLabels();
    } else if (currentState == NetworkState::ConnectedStaLocal || currentState == NetworkState::ConnectedStaInternet) {
        refreshConnectionLabel();
    }
}

// ============================================================
// AP Event Handlers
// ============================================================

void apRefreshClientList() {
    int n = network_manager::getApClientCount();
    if (n == lastApStationCount) return;
    lastApStationCount = n;

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

void apPollCb(lv_timer_t*) {
    if (!network_manager::isApEnabled()) return;
    apRefreshClientList();
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
    bool valid = strlen(ssid) > 0 && (!secure || strlen(password) >= kMinApPasswordLen);

    if (valid) lv_obj_clear_state(apToggle, LV_STATE_DISABLED);
    else lv_obj_add_state(apToggle, LV_STATE_DISABLED);
}

void apSecureSwitchEventCb(lv_event_t*) {
    bool secure = lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED);
    if (secure) lv_obj_clear_state(apPasswordInput, LV_STATE_DISABLED);
    else lv_obj_add_state(apPasswordInput, LV_STATE_DISABLED);
    apUpdateToggleEnabled();
}

void apFormFieldChangedCb(lv_event_t*) { apUpdateToggleEnabled(); }

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

        network_manager::startAp(ssid, password, secure);
        lastApStationCount = -1;
        lv_obj_add_state(apSsidInput, LV_STATE_DISABLED);
        lv_obj_add_state(apSecureSwitch, LV_STATE_DISABLED);
        lv_obj_add_state(apPasswordInput, LV_STATE_DISABLED);
        lv_label_set_text(apInfoLabel, "Access point running.");
        apRefreshClientList();
    } else {
        network_manager::stopAp();
        lastApStationCount = -1;
        lv_obj_clear_state(apSsidInput, LV_STATE_DISABLED);
        lv_obj_clear_state(apSecureSwitch, LV_STATE_DISABLED);
        if (lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED)) {
            lv_obj_clear_state(apPasswordInput, LV_STATE_DISABLED);
        }
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

void setMode(bool ap) {
    if (ap) {
        lv_obj_add_state(apTabBtn, LV_STATE_CHECKED);
        lv_obj_clear_state(clientTabBtn, LV_STATE_CHECKED);
        lv_obj_clear_flag(apPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clientPanel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_state(clientTabBtn, LV_STATE_CHECKED);
        lv_obj_clear_state(apTabBtn, LV_STATE_CHECKED);
        lv_obj_clear_flag(clientPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(apPanel, LV_OBJ_FLAG_HIDDEN);
    }
}

void clientTabCb(lv_event_t*) { setMode(false); }
void apTabCb(lv_event_t*) { setMode(true); }

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* scr = lv_obj_create(parent);
    lv_obj_set_size(scr, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(scr, 8, 0); // Increased screen padding to stop edge touching
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 10, 0); // Gap between tabs and panels

    // ---- Tab switcher ----
    lv_obj_t* tabRow = lv_obj_create(scr);
    lv_obj_remove_style_all(tabRow);
    lv_obj_set_size(tabRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tabRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabRow, 8, 0); // Spacing between tab buttons

    clientTabBtn = lv_btn_create(tabRow);
    lv_obj_set_flex_grow(clientTabBtn, 1);
    lv_obj_add_event_cb(clientTabBtn, clientTabCb, LV_EVENT_CLICKED, nullptr);
    clientTabLabel = lv_label_create(clientTabBtn);
    lv_label_set_text(clientTabLabel, "Client");
    lv_obj_center(clientTabLabel);

    apTabBtn = lv_btn_create(tabRow);
    lv_obj_set_flex_grow(apTabBtn, 1);
    lv_obj_add_event_cb(apTabBtn, apTabCb, LV_EVENT_CLICKED, nullptr);
    apTabLabel = lv_label_create(apTabBtn);
    lv_label_set_text(apTabLabel, "Access Point");
    lv_obj_center(apTabLabel);

    // ---- Client panel ----
    clientPanel = lv_obj_create(scr);
    lv_obj_remove_style_all(clientPanel);
    lv_obj_set_size(clientPanel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(clientPanel, 1);
    lv_obj_set_flex_flow(clientPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(clientPanel, 10, 0); // Gap between connection, list, and buttons

    // 1. Prominent Active Connection Panel (Hidden by default)
    activeConnPanel = lv_obj_create(clientPanel);
    lv_obj_set_width(activeConnPanel, lv_pct(100));
    lv_obj_set_flex_flow(activeConnPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(activeConnPanel, 12, 0);
    lv_obj_set_style_pad_row(activeConnPanel, 4, 0);
    lv_obj_set_style_bg_color(activeConnPanel, lv_palette_lighten(LV_PALETTE_BLUE, 4), 0); // Light highlight
    lv_obj_add_flag(activeConnPanel, LV_OBJ_FLAG_HIDDEN);

    activeSsidLabel = lv_label_create(activeConnPanel);
    lv_obj_set_style_text_font(activeSsidLabel, &lv_font_montserrat_18, 0); // Larger font if available

    activeStatusLabel = lv_label_create(activeConnPanel);
    activeRssiLabel = lv_label_create(activeConnPanel);

    // 2. Scan Label
    scanLabel = lv_label_create(clientPanel);
    lv_obj_set_width(scanLabel, lv_pct(100));
    lv_label_set_text(scanLabel, "Tap Scan to find networks.");

    // 3. Network List
    networkList = lv_list_create(clientPanel);
    lv_obj_set_width(networkList, lv_pct(100));
    lv_obj_set_flex_grow(networkList, 1);
    lv_list_add_text(networkList, "No networks yet");

    // 4. Scan Button (Moved to bottom)
    scanBtn = lv_btn_create(clientPanel);
    lv_obj_set_width(scanBtn, lv_pct(100));
    lv_obj_add_event_cb(scanBtn, scanEventCb, LV_EVENT_CLICKED, nullptr);
    scanBtnLabel = lv_label_create(scanBtn);
    lv_label_set_text(scanBtnLabel, LV_SYMBOL_REFRESH " Scan");
    lv_obj_center(scanBtnLabel);


    // ---- Access point panel ----
    apPanel = lv_obj_create(scr);
    lv_obj_remove_style_all(apPanel);
    lv_obj_set_size(apPanel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(apPanel, 1);
    lv_obj_set_flex_flow(apPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(apPanel, 10, 0); // Gap between AP elements
    lv_obj_add_flag(apPanel, LV_OBJ_FLAG_HIDDEN);

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

    apPasswordInput = lv_textarea_create(pwdRow);
    lv_textarea_set_one_line(apPasswordInput, true);
    lv_textarea_set_placeholder_text(apPasswordInput, "Access Point Password");
    lv_obj_set_flex_grow(apPasswordInput, 1);
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
    lv_obj_set_width(apClientList, lv_pct(100));
    lv_obj_set_flex_grow(apClientList, 1);

    apKeyboard = lv_keyboard_create(scr);
    lv_obj_add_flag(apKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(apKeyboard, apKeyboardEventCb, LV_EVENT_ALL, nullptr);

    // Initial persistence sync
    char savedSsid[33], savedPass[64];
    bool savedSecure;
    network_manager::getSavedApSettings(savedSsid, sizeof(savedSsid), savedSecure, savedPass, sizeof(savedPass));
    if (savedSsid[0] != '\0') lv_textarea_set_text(apSsidInput, savedSsid);
    if (savedSecure) lv_obj_add_state(apSecureSwitch, LV_STATE_CHECKED);
    else lv_obj_clear_state(apSecureSwitch, LV_STATE_CHECKED);
    if (savedPass[0] != '\0') lv_textarea_set_text(apPasswordInput, savedPass);

    apSecureSwitchEventCb(nullptr);
    setMode(false);
    updateTabLabels();

    pollTimer = lv_timer_create(pollCb, kPollIntervalMs, nullptr);
    apPollTimer = lv_timer_create(apPollCb, kApPollIntervalMs, nullptr);

    return scr;
}

} // namespace screen_wifi
