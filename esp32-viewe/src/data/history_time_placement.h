#pragma once

#include <cstddef>
#include <cstdint>

// Pure session-placement policy shared by the history reader and native tests.
// Measurement files remain immutable; these placements exist only for one
// query and map each session's first retained minute onto the selected axis.
namespace history_time_placement {

constexpr int64_t kMinuteMs = 60000LL;

enum class Kind : uint8_t {
    Unresolved = 0,
    Direct,
    Inferred,
    Assumed,
};

struct Session {
    uint32_t id = 0;
    uint32_t firstMinute = 0;
    uint32_t endMinute = 0; // exclusive
    int64_t startTimeMs = 0;
    Kind kind = Kind::Unresolved;
};

int64_t durationMs(const Session& session);
int64_t endTimeMs(const Session& session);

// Resolve every unresolved block that is surrounded by placed sessions when
// its combined unexplained time is non-negative and no larger than maxSlackMs.
// Slack is divided evenly across all restart boundaries, matching History V1.
void inferBounded(Session* sessions, size_t count, int64_t maxSlackMs);

// Place remaining blocks with a fixed gap between retained session extents.
// Prefer the later fixed boundary so recent data stays adjacent to the current
// boot. A block that cannot fit between two fixed endpoints is left unresolved.
void assumeUnresolved(Session* sessions, size_t count, int64_t restartGapMs);

} // namespace history_time_placement
