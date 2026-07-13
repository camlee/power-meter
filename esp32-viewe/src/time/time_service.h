#pragma once

#include <stdint.h>

namespace time_service {

enum class AnchorSource : uint8_t {
    Unknown = 0,
    Ntp,
    Browser,
    Peer,
    Rtc,
};

struct Anchor {
    uint32_t sessionId;
    uint64_t monotonicUs;
    int64_t unixTimeMs;
    AnchorSource source;
    int16_t utcOffsetMinutes;
    uint32_t uncertaintyMs;
};

// Starts a persistent monotonic boot session and loads the small V3 anchor
// ledger from LittleFS. LittleFS must already be mounted. Safe to call twice.
void init();

uint32_t currentSessionId();
uint64_t monotonicUs();

// Records a relationship between this boot's monotonic clock and civil time.
// NTP and local-browser synchronization both enter through this API. A valid
// browser-provided offset also becomes the persisted fixed local UTC offset.
bool submitAnchor(int64_t unixTimeMs, AnchorSource source,
                  int16_t utcOffsetMinutes, uint32_t uncertaintyMs = 0);

bool hasCurrentTime();
bool getCurrentAnchor(Anchor& out);
bool getAnchorForSession(uint32_t sessionId, Anchor& out);

// Resolves a sample from any retained anchored boot session. The raw UTC
// instant is returned; use the anchor's offset (or utcOffsetMinutes()) for
// calendar bucketing.
bool resolveUnixTimeMs(uint32_t sessionId, uint64_t sampleMonotonicUs,
                       int64_t& unixTimeMsOut, Anchor* anchorOut = nullptr);

int16_t utcOffsetMinutes();
bool setUtcOffsetMinutes(int16_t offsetMinutes);
const char* sourceName(AnchorSource source);

// Used by the explicit Usage Data reset workflow.
bool clearHistoryAnchors();

} // namespace time_service
