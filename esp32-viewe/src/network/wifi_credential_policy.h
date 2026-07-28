#pragma once

#include <stddef.h>
#include <string.h>

namespace network_manager {

enum class ApPasswordAction {
    Keep,
    Replace,
    Remove,
};

enum class ApSettingsValidation {
    Valid,
    InvalidSsid,
    InvalidPasswordAction,
    InvalidPassword,
};

inline bool validWifiSettingText(const char* value, size_t minimum,
                                 size_t maximum) {
    if (!value) return minimum == 0;
    const size_t length = strlen(value);
    if (length < minimum || length > maximum) return false;
    for (size_t i = 0; i < length; ++i) {
        if (static_cast<unsigned char>(value[i]) < 0x20) return false;
    }
    return true;
}

inline ApSettingsValidation validateApSettings(
    const char* ssid, bool secure, ApPasswordAction passwordAction,
    const char* replacementPassword, bool savedPasswordConfigured) {
    if (!validWifiSettingText(ssid, 1, 32)) {
        return ApSettingsValidation::InvalidSsid;
    }
    switch (passwordAction) {
        case ApPasswordAction::Keep:
            if (!secure || !savedPasswordConfigured || replacementPassword) {
                return ApSettingsValidation::InvalidPasswordAction;
            }
            return ApSettingsValidation::Valid;
        case ApPasswordAction::Replace:
            if (!secure) return ApSettingsValidation::InvalidPasswordAction;
            return validWifiSettingText(replacementPassword, 8, 63)
                       ? ApSettingsValidation::Valid
                       : ApSettingsValidation::InvalidPassword;
        case ApPasswordAction::Remove:
            if (secure || replacementPassword) {
                return ApSettingsValidation::InvalidPasswordAction;
            }
            return ApSettingsValidation::Valid;
    }
    return ApSettingsValidation::InvalidPasswordAction;
}

} // namespace network_manager
