#pragma once

#include <stdint.h>

namespace internet_update_service {

enum class State : uint8_t {
    Idle,
    WaitingForNetwork,
    WaitingForTime,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Verifying,
    Rebooting,
    Failed,
    BlockedAfterRollback,
};

struct Status {
    State state = State::Idle;
    bool automatic = true;
    bool busy = false;
    uint8_t progressPercent = 0;
    int64_t updateDateUnixSeconds = 0;
    int64_t lastCheckUnixSeconds = 0;
    int64_t nextCheckUnixSeconds = 0;
    char currentVersion[64]{};
    char availableVersion[64]{};
    char blockedVersion[64]{};
    char error[128]{};
};

void begin();
void update();
bool getStatus(Status& out);
const char* stateName(State state);

// Manual checks are check-only. Installation is a separate explicit action.
bool requestCheck();
bool requestInstall();
bool setAutomatic(bool enabled);

} // namespace internet_update_service
