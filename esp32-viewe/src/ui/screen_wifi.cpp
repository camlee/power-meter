#include "screen_wifi.h"
#include "nav_bar.h"
#include "screen_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>

namespace screen_wifi {
namespace {

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kPollIntervalMs = 200;
constexpr uint32_t kApPollIntervalMs = 1000;
constexpr int kMinApPasswordLen = 8;
constexpr int kMaxSavedNetworks = 8;

enum class Op { Idle, Scanning, Connecting };

// ---- Shared / tab switcher ----
lv_obj_t* clientTabBtn = nullptr;
lv_obj_t* apTabBtn = nullptr;
lv_obj_t* clientTabLabel = nullptr;
lv_obj_t* apTabLabel = nullptr;
lv_obj_t* clientPanel = nullptr;
lv_obj_t* apPanel = nullptr;

// ---- Client (station) mode ----
lv_obj_t* connectionLabel = nullptr;
lv_obj_t* scanLabel = nullptr;
lv_obj_t* scanBtn = nullptr;
lv_obj_t* scanBtnLabel = nullptr;
lv_obj_t* networkList = nullptr;
lv_timer_t* pollTimer = nullptr;

lv_obj_t* pwdOverlay = nullptr;
lv_obj_t* pwdTextarea = nullptr;

char selectedSsid[33] = "";
char pendingSsid[33] = "";
char lastConnectPassword[64] = "";
Op op = Op::Idle;
uint32_t opStartedAt = 0;
bool lastConnectedState = false;

struct NetworkInfo {
    char ssid[33];
    bool secured;
};

// ---- Access point mode ----
bool apRunning = false;
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

// ============================================================
// Persistence (NVS via Preferences)
// ============================================================
//
// Namespace "wifi_net" stores a small ring of previously-used client
// credentials so we can prefill (and eventually auto-fill) the password
// prompt for networks we've connected to before:
//   cnt  (u8)   - number of slots in use (0..kMaxSavedNetworks)
//   next (u8)   - next slot to overwrite once the ring is full
//   s0..sN      - ssid stored at slot N
//   p0..pN      - password stored at slot N
//
// Namespace "wifi_ap" stores the last-configured Access Point form values.

int findSavedNetworkSlot(Preferences& prefs, uint8_t count, const char* ssid) {
    char key[4];
    for (uint8_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "s%u", i);
        String stored = prefs.getString(key, "");
        if (stored.equals(ssid)) return i;
    }
    return -1;
}

bool loadNetworkCredential(const char* ssid, char* outPass, size_t outLen) {
    if (!ssid || ssid[0] == '\0' || outLen == 0) return false;
    Preferences prefs;
    if (!prefs.begin("wifi_net", true /* read-only */)) return false;

    uint8_t count = prefs.getUChar("cnt", 0);
    int slot = findSavedNetworkSlot(prefs, count, ssid);
    bool found = false;
    if (slot >= 0) {
        char key[4];
        snprintf(key, sizeof(key), "p%d", slot);
        String pass = prefs.getString(key, "");
        strncpy(outPass, pass.c_str(), outLen - 1);
        outPass[outLen - 1] = '\0';
        found = true;
    }
    prefs.end();
    return found;
}

void saveNetworkCredential(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0' || !password || password[0] == '\0') return;

    Preferences prefs;
    if (!prefs.begin("wifi_net", false)) return;

    uint8_t count = prefs.getUChar("cnt", 0);
    int slot = findSavedNetworkSlot(prefs, count, ssid);

    if (slot < 0) {
        if (count < kMaxSavedNetworks) {
            slot = count;
            count++;
            prefs.putUChar("cnt", count);
        } else {
            uint8_t next = prefs.getUChar("next", 0);
            slot = next;
            next = (next + 1) % kMaxSavedNetworks;
            prefs.putUChar("next", next);
        }
        char ssidKey[4];
        snprintf(ssidKey, sizeof(ssidKey), "s%d", slot);
        prefs.putString(ssidKey, ssid);
    }

    char passKey[4];
    snprintf(passKey, sizeof(passKey), "p%d", slot);
    prefs.putString(passKey, password);
    prefs.end();
}

void loadApSettings(char* ssidOut, size_t ssidLen, bool& secureOut, char* passOut, size_t passLen) {
    ssidOut[0] = '\0';
    passOut[0] = '\0';
    secureOut = true;

    Preferences prefs;
    if (!prefs.begin("wifi_ap", true /* read-only */)) return;

    String ssid = prefs.getString("ssid", "");
    strncpy(ssidOut, ssid.c_str(), ssidLen - 1);
    ssidOut[ssidLen - 1] = '\0';

    secureOut = prefs.getBool("secure", true);

    String pass = prefs.getString("pass", "");
    strncpy(passOut, pass.c_str(), passLen - 1);
    passOut[passLen - 1] = '\0';

    prefs.end();
}

void saveApSettings(const char* ssid, bool secure, const char* password) {
    Preferences prefs;
    if (!prefs.begin("wifi_ap", false)) return;
    prefs.putString("ssid", ssid);
    prefs.putBool("secure", secure);
    prefs.putString("pass", secure ? password : "");
    prefs.end();
}

// ============================================================
// Connection status (shared source of truth, never hidden by scan)
// ============================================================

bool isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool isConnectedTo(const char* ssid) {
    return isConnected() && strcmp(WiFi.SSID().c_str(), ssid) == 0;
}

// ============================================================
// Tab button state (checkmark when connected / AP running)
// ============================================================

void updateTabLabels() {
    if (clientTabLabel) {
        lv_label_set_text(clientTabLabel, isConnected() ? LV_SYMBOL_OK " Station" : "Station");
    }
        char buffer[128];
        if (apRunning) {
            // Display client count and status when running
            snprintf(buffer, sizeof(buffer), "%s (%d Clients)", LV_SYMBOL_OK, lastApStationCount);
            lv_label_set_text(apTabLabel, buffer);
        } else {
            lv_label_set_text(apTabLabel, "Access Point");
        }
}

// ============================================================
// Scan list icon sync (updates in place, no rescan needed)
// ============================================================

void updateListConnectionIcons() {
    if (!networkList) return;
    uint32_t cnt = lv_obj_get_child_cnt(networkList);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* btn = lv_obj_get_child(networkList, i);
        auto* info = (NetworkInfo*)lv_obj_get_user_data(btn);
        if (!info) continue; // plain text items (e.g. "No networks found")

        bool connected = isConnectedTo(info->ssid);
        lv_obj_t* icon = lv_obj_get_child(btn, 0);
        if (icon) {
            lv_img_set_src(icon, connected ? LV_SYMBOL_OK : LV_SYMBOL_WIFI);
        }
        if (connected) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(btn, LV_STATE_CHECKED);
        }
    }
}

void syncConnectionDependentUI() {
    bool connected = isConnected();
    if (connected != lastConnectedState) {
        lastConnectedState = connected;
        updateListConnectionIcons();
        updateTabLabels();
    }
}

void refreshConnectionLabel() {
    if (op == Op::Connecting) {
        char buf[72];
        snprintf(buf, sizeof(buf), "Connecting to %s...", selectedSsid);
        lv_label_set_text(connectionLabel, buf);
        return;
    }
    if (isConnected()) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Connected: %s (%ld dBm)",
                 WiFi.SSID().c_str(), static_cast<long>(WiFi.RSSI()));
        lv_label_set_text(connectionLabel, buf);
    } else {
        lv_label_set_text(connectionLabel, "Not connected.");
    }
}

// ============================================================
// Client mode: scan + connect
// ============================================================

void setScanBusy(bool busy) {
    if (!scanBtn || !scanBtnLabel) return;
    if (busy) {
        lv_obj_add_state(scanBtn, LV_STATE_DISABLED);
        lv_label_set_text(scanBtnLabel, "Scanning...");
    } else {
        lv_obj_clear_state(scanBtn, LV_STATE_DISABLED);
        lv_label_set_text(scanBtnLabel, LV_SYMBOL_REFRESH " Scan");
    }
}

void closePasswordPrompt() {
    if (pwdOverlay) {
        lv_obj_del(pwdOverlay);
        pwdOverlay = nullptr;
        pwdTextarea = nullptr;
    }
}

void beginConnect(const char* ssid, const char* password) {
    strncpy(selectedSsid, ssid, sizeof(selectedSsid) - 1);
    selectedSsid[sizeof(selectedSsid) - 1] = '\0';

    if (password && password[0] != '\0') {
        strncpy(lastConnectPassword, password, sizeof(lastConnectPassword) - 1);
        lastConnectPassword[sizeof(lastConnectPassword) - 1] = '\0';
        WiFi.begin(selectedSsid, password);
    } else {
        lastConnectPassword[0] = '\0';
        WiFi.begin(selectedSsid);
    }
    op = Op::Connecting;
    opStartedAt = millis();
    refreshConnectionLabel();
}

void pwdConnectCb(lv_event_t*) {
    const char* password = lv_textarea_get_text(pwdTextarea);
    beginConnect(pendingSsid, password);
    closePasswordPrompt();
}

void pwdCancelCb(lv_event_t*) {
    closePasswordPrompt();
}

void pwdKeyboardEventCb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        pwdConnectCb(e);
    } else if (code == LV_EVENT_CANCEL) {
        pwdCancelCb(e);
    }
}

void showPasswordPrompt(const char* ssid) {
    strncpy(pendingSsid, ssid, sizeof(pendingSsid) - 1);
    pendingSsid[sizeof(pendingSsid) - 1] = '\0';

    pwdOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pwdOverlay);
    lv_obj_set_size(pwdOverlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(pwdOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pwdOverlay, LV_OPA_50, 0);
    lv_obj_set_flex_flow(pwdOverlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pwdOverlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* panel = lv_obj_create(pwdOverlay);
    lv_obj_set_size(panel, lv_pct(85), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);

    char title[48];
    snprintf(title, sizeof(title), "Password for %s", ssid);
    lv_obj_t* titleLabel = lv_label_create(panel);
    lv_obj_set_width(titleLabel, lv_pct(100));
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(titleLabel, title);

    pwdTextarea = lv_textarea_create(panel);
    lv_textarea_set_one_line(pwdTextarea, true);
    lv_obj_set_width(pwdTextarea, lv_pct(100));

    // Prefill with a previously-saved password for this network, if any.
    char savedPass[64];
    if (loadNetworkCredential(ssid, savedPass, sizeof(savedPass))) {
        lv_textarea_set_text(pwdTextarea, savedPass);
    }

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_remove_style_all(btnRow);
    lv_obj_set_size(btnRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btnRow, 8, 0);

    lv_obj_t* cancelBtn = lv_btn_create(btnRow);
    lv_obj_set_flex_grow(cancelBtn, 1);
    lv_obj_add_event_cb(cancelBtn, pwdCancelCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancelLabel = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLabel, "Cancel");
    lv_obj_center(cancelLabel);

    lv_obj_t* connectBtn = lv_btn_create(btnRow);
    lv_obj_set_flex_grow(connectBtn, 1);
    lv_obj_add_event_cb(connectBtn, pwdConnectCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* connectLabel = lv_label_create(connectBtn);
    lv_label_set_text(connectLabel, "Connect");
    lv_obj_center(connectLabel);

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

    if (info->secured) {
        showPasswordPrompt(info->ssid);
    } else {
        beginConnect(info->ssid, nullptr);
    }
}

void rebuildListFromScan(int n) {
    lv_obj_clean(networkList);
    setScanBusy(false);

    if (n <= 0) {
        lv_list_add_text(networkList, "No networks found");
        lv_label_set_text(scanLabel, "No networks found. Tap Scan to try again.");
        WiFi.scanDelete();
        return;
    }

    for (int i = 0; i < n; i++) {
        bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        bool connected = isConnectedTo(WiFi.SSID(i).c_str());

        char rowText[80];
        snprintf(rowText, sizeof(rowText), "%s", WiFi.SSID(i).c_str());

        const char* icon = connected ? LV_SYMBOL_OK : LV_SYMBOL_WIFI;
        lv_obj_t* btn = lv_list_add_btn(networkList, icon, rowText);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);

        auto* info = (NetworkInfo*)malloc(sizeof(NetworkInfo));
        strncpy(info->ssid, WiFi.SSID(i).c_str(), sizeof(info->ssid) - 1);
        info->ssid[sizeof(info->ssid) - 1] = '\0';
        info->secured = secured;

        lv_obj_set_user_data(btn, info);
        lv_obj_add_event_cb(btn, rowClickCb, LV_EVENT_CLICKED, info);
        lv_obj_add_event_cb(btn, rowDeleteCb, LV_EVENT_DELETE, nullptr);

        if (connected) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        }
    }

    char status[48];
    snprintf(status, sizeof(status), "Found %d network%s.", n, n == 1 ? "" : "s");
    lv_label_set_text(scanLabel, status);
    WiFi.scanDelete();
}

void scanEventCb(lv_event_t*) {
    // Note: only the scan list/label reset here - connectionLabel is left
    // alone so "Connected: X" stays visible while scanning runs.
    lv_obj_clean(networkList);
    lv_list_add_text(networkList, "Scanning...");
    setScanBusy(true);
    lv_label_set_text(scanLabel, "Scanning for WiFi networks...");
    WiFi.scanNetworks(true /* async */);
    op = Op::Scanning;
}

void pollCb(lv_timer_t*) {
    if (op == Op::Scanning) {
        int n = WiFi.scanComplete();
        if (n != WIFI_SCAN_RUNNING) {
            op = Op::Idle;
            rebuildListFromScan(n);
            refreshConnectionLabel();
        }
    } else if (op == Op::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            op = Op::Idle;
            refreshConnectionLabel();
            // Persist the credential that got us connected (skip open networks).
            if (lastConnectPassword[0] != '\0') {
                saveNetworkCredential(selectedSsid, lastConnectPassword);
            }
        } else if (millis() - opStartedAt > kConnectTimeoutMs) {
            op = Op::Idle;
            char buf[72];
            snprintf(buf, sizeof(buf), "Could not connect to %s.", selectedSsid);
            lv_label_set_text(connectionLabel, buf);
        }
    } else {
        // Idle: keep the connection line live (RSSI updates, catches
        // unexpected disconnects) without touching scan state.
        refreshConnectionLabel();
    }

    // Cheap every-tick check: flips the scan-list checkmarks and the tab
    // icon exactly once, right when the connection state actually changes.
    syncConnectionDependentUI();
}

// ============================================================
// Access point mode
// ============================================================

void apRefreshClientList() {
    int n = WiFi.softAPgetStationNum();
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

    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK) {
        lv_list_add_text(apClientList, "Unable to read client list");
        return;
    }
    for (int i = 0; i < staList.num; i++) {
        const uint8_t* mac = staList.sta[i].mac;
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        lv_list_add_text(apClientList, macStr);
    }
}

void apPollCb(lv_timer_t*) {
    if (!apRunning) return;
    apRefreshClientList();
}

// Enables/disables the AP on/off toggle based on whether the form currently
// holds a startable configuration. Always left enabled while running so the
// user can turn the AP off regardless of what they've typed since.
void apUpdateToggleEnabled() {
    if (!apToggle) return;
    if (apRunning) {
        lv_obj_clear_state(apToggle, LV_STATE_DISABLED);
        return;
    }

    const char* ssid = lv_textarea_get_text(apSsidInput);
    bool secure = lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED);
    const char* password = lv_textarea_get_text(apPasswordInput);
    bool valid = strlen(ssid) > 0 && (!secure || strlen(password) >= kMinApPasswordLen);

    if (valid) {
        lv_obj_clear_state(apToggle, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(apToggle, LV_STATE_DISABLED);
    }
}

// Toggle semantics: ON = secure (password required), OFF = open network.
void apSecureSwitchEventCb(lv_event_t*) {
    bool secure = lv_obj_has_state(apSecureSwitch, LV_STATE_CHECKED);
    if (secure) {
        lv_obj_clear_state(apPasswordInput, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(apPasswordInput, LV_STATE_DISABLED);
    }
    apUpdateToggleEnabled();
}

void apFormFieldChangedCb(lv_event_t*) {
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

        if (strlen(ssid) == 0) {
            lv_label_set_text(apInfoLabel, "Enter a network name first.");
            lv_obj_clear_state(apToggle, LV_STATE_CHECKED);
            return;
        }
        if (secure && strlen(password) < kMinApPasswordLen) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Password must be at least %d characters.", kMinApPasswordLen);
            lv_label_set_text(apInfoLabel, buf);
            lv_obj_clear_state(apToggle, LV_STATE_CHECKED);
            return;
        }

        WiFi.mode(WIFI_AP_STA);
        bool ok = WiFi.softAP(ssid, secure ? password : nullptr);
        if (!ok) {
            lv_label_set_text(apInfoLabel, "Failed to start access point.");
            lv_obj_clear_state(apToggle, LV_STATE_CHECKED);
            WiFi.mode(WIFI_STA);
            return;
        }

        apRunning = true;
        lastApStationCount = -1;
        lv_obj_add_state(apSsidInput, LV_STATE_DISABLED);
        lv_obj_add_state(apSecureSwitch, LV_STATE_DISABLED);
        lv_obj_add_state(apPasswordInput, LV_STATE_DISABLED);

        char buf[96];
        snprintf(buf, sizeof(buf), "\"%s\" running at %s", ssid, WiFi.softAPIP().toString().c_str());
        lv_label_set_text(apInfoLabel, buf);
        apRefreshClientList();

        saveApSettings(ssid, secure, password);
    } else {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        apRunning = false;
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
// Tab switching
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

lv_obj_t* create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 12, 0);
    lv_obj_set_style_pad_row(scr, 10, 0);

    nav_bar::create(scr, ScreenId::WiFi);

    lastConnectedState = isConnected();

    // ---- Tab switcher ----
    lv_obj_t* tabRow = lv_obj_create(scr);
    lv_obj_remove_style_all(tabRow);
    lv_obj_set_size(tabRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tabRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabRow, 8, 0);

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
    lv_obj_set_style_pad_row(clientPanel, 8, 0);

    connectionLabel = lv_label_create(clientPanel);
    lv_obj_set_width(connectionLabel, lv_pct(100));
    lv_label_set_long_mode(connectionLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(connectionLabel, "Not connected.");

    scanBtn = lv_btn_create(clientPanel);
    lv_obj_set_width(scanBtn, lv_pct(100));
    lv_obj_add_event_cb(scanBtn, scanEventCb, LV_EVENT_CLICKED, nullptr);
    scanBtnLabel = lv_label_create(scanBtn);
    lv_label_set_text(scanBtnLabel, LV_SYMBOL_REFRESH " Scan");
    lv_obj_center(scanBtnLabel);

    scanLabel = lv_label_create(clientPanel);
    lv_obj_set_width(scanLabel, lv_pct(100));
    lv_obj_set_style_text_color(scanLabel, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_long_mode(scanLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(scanLabel, "Tap Scan to find networks.");

    networkList = lv_list_create(clientPanel);
    lv_obj_set_width(networkList, lv_pct(100));
    lv_obj_set_flex_grow(networkList, 1);
    lv_list_add_text(networkList, "No networks yet");

    // ---- Access point panel ----
    apPanel = lv_obj_create(scr);
    lv_obj_remove_style_all(apPanel);
    lv_obj_set_size(apPanel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(apPanel, 1);
    lv_obj_set_flex_flow(apPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(apPanel, 6, 0);
    lv_obj_add_flag(apPanel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* ssidLabel = lv_label_create(apPanel);
    lv_label_set_text(ssidLabel, "Network name");

    apSsidInput = lv_textarea_create(apPanel);
    lv_textarea_set_one_line(apSsidInput, true);
    lv_textarea_set_max_length(apSsidInput, 32);
    lv_obj_set_width(apSsidInput, lv_pct(100));
    lv_obj_add_event_cb(apSsidInput, apInputFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(apSsidInput, apInputDefocusCb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(apSsidInput, apFormFieldChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

    // ---- Password row: label above, [secure toggle][password field] combined on one line ----
    lv_obj_t* pwdLabel = lv_label_create(apPanel);
    lv_label_set_text(pwdLabel, "Password");

    lv_obj_t* pwdRow = lv_obj_create(apPanel);
    lv_obj_remove_style_all(pwdRow);
    lv_obj_set_size(pwdRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pwdRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pwdRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pwdRow, 8, 0);

    apSecureSwitch = lv_switch_create(pwdRow);
    lv_obj_add_state(apSecureSwitch, LV_STATE_CHECKED); // default: secure network
    lv_obj_add_event_cb(apSecureSwitch, apSecureSwitchEventCb, LV_EVENT_VALUE_CHANGED, nullptr);

    apPasswordInput = lv_textarea_create(pwdRow);
    lv_textarea_set_one_line(apPasswordInput, true);
    lv_textarea_set_max_length(apPasswordInput, 63);
    lv_obj_set_flex_grow(apPasswordInput, 1);
    lv_obj_add_event_cb(apPasswordInput, apInputFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(apPasswordInput, apInputDefocusCb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(apPasswordInput, apFormFieldChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

    // ---- On/off toggle instead of a Start/Stop button ----
    lv_obj_t* apToggleRow = lv_obj_create(apPanel);
    lv_obj_remove_style_all(apToggleRow);
    lv_obj_set_size(apToggleRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(apToggleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(apToggleRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(apToggleRow, 8, 0);

    lv_obj_t* apToggleLabel = lv_label_create(apToggleRow);
    lv_label_set_text(apToggleLabel, "Enable Access Point");

    apToggle = lv_switch_create(apToggleRow);
    lv_obj_add_event_cb(apToggle, apToggleEventCb, LV_EVENT_VALUE_CHANGED, nullptr);

    apInfoLabel = lv_label_create(apPanel);
    lv_obj_set_width(apInfoLabel, lv_pct(100));
    lv_label_set_long_mode(apInfoLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(apInfoLabel, "Access point stopped.");

    apCountLabel = lv_label_create(apPanel);
    lv_label_set_text(apCountLabel, "0 devices connected");

    apClientList = lv_list_create(apPanel);
    lv_obj_set_width(apClientList, lv_pct(100));
    lv_obj_set_flex_grow(apClientList, 1);
    lv_list_add_text(apClientList, "Access point not running");

    apKeyboard = lv_keyboard_create(scr);
    lv_obj_add_flag(apKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(apKeyboard, apKeyboardEventCb, LV_EVENT_ALL, nullptr);

    // ---- Load persisted AP settings and sync dependent widget states ----
    {
        char savedSsid[33];
        char savedPass[64];
        bool savedSecure;
        loadApSettings(savedSsid, sizeof(savedSsid), savedSecure, savedPass, sizeof(savedPass));

        if (savedSsid[0] != '\0') {
            lv_textarea_set_text(apSsidInput, savedSsid);
        }
        if (savedSecure) {
            lv_obj_add_state(apSecureSwitch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(apSecureSwitch, LV_STATE_CHECKED);
        }
        if (savedPass[0] != '\0') {
            lv_textarea_set_text(apPasswordInput, savedPass);
        }
    }
    apSecureSwitchEventCb(nullptr); // applies password-field enable state + toggle validity

    setMode(false); // default to Client tab
    updateTabLabels();

    pollTimer = lv_timer_create(pollCb, kPollIntervalMs, nullptr);
    apPollTimer = lv_timer_create(apPollCb, kApPollIntervalMs, nullptr);
    refreshConnectionLabel();

    return scr;
}

} // namespace screen_wifi
