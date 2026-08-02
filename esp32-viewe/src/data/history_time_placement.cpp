#include "history_time_placement.h"

#include <algorithm>

namespace history_time_placement {
namespace {

bool placed(const Session& session) {
    return session.kind != Kind::Unresolved;
}

} // namespace

int64_t durationMs(const Session& session) {
    return session.endMinute > session.firstMinute
        ? static_cast<int64_t>(session.endMinute - session.firstMinute) * kMinuteMs
        : 0;
}

int64_t endTimeMs(const Session& session) {
    return session.startTimeMs + durationMs(session);
}

void inferBounded(Session* sessions, size_t count, int64_t maxSlackMs) {
    if (!sessions || count < 3 || maxSlackMs < 0) return;
    size_t blockStart = 0;
    while (blockStart < count) {
        while (blockStart < count && placed(sessions[blockStart])) ++blockStart;
        if (blockStart == count) break;
        size_t blockEnd = blockStart;
        while (blockEnd + 1 < count && !placed(sessions[blockEnd + 1])) ++blockEnd;
        if (blockStart == 0 || blockEnd + 1 >= count) {
            blockStart = blockEnd + 1;
            continue;
        }

        const Session& previous = sessions[blockStart - 1];
        const Session& next = sessions[blockEnd + 1];
        int64_t measuredMs = 0;
        for (size_t i = blockStart; i <= blockEnd; ++i) measuredMs += durationMs(sessions[i]);
        const int64_t slackMs = next.startTimeMs - endTimeMs(previous) - measuredMs;
        if (slackMs >= 0 && slackMs <= maxSlackMs) {
            const size_t boundaryCount = blockEnd - blockStart + 2;
            const int64_t gapMs = slackMs / static_cast<int64_t>(boundaryCount);
            int64_t cursor = endTimeMs(previous) + gapMs;
            for (size_t i = blockStart; i <= blockEnd; ++i) {
                sessions[i].startTimeMs = cursor;
                sessions[i].kind = Kind::Inferred;
                cursor = endTimeMs(sessions[i]) + gapMs;
            }
        }
        blockStart = blockEnd + 1;
    }
}

void assumeUnresolved(Session* sessions, size_t count, int64_t restartGapMs) {
    if (!sessions || !count || restartGapMs < 0) return;
    size_t blockStart = 0;
    while (blockStart < count) {
        while (blockStart < count && placed(sessions[blockStart])) ++blockStart;
        if (blockStart == count) break;
        size_t blockEnd = blockStart;
        while (blockEnd + 1 < count && !placed(sessions[blockEnd + 1])) ++blockEnd;
        const bool havePrevious = blockStart > 0 && placed(sessions[blockStart - 1]);
        const bool haveNext = blockEnd + 1 < count && placed(sessions[blockEnd + 1]);

        if (haveNext) {
            int64_t cursor = sessions[blockEnd + 1].startTimeMs - restartGapMs;
            for (size_t offset = 0; offset <= blockEnd - blockStart; ++offset) {
                const size_t i = blockEnd - offset;
                sessions[i].startTimeMs = cursor - durationMs(sessions[i]);
                cursor = sessions[i].startTimeMs - restartGapMs;
            }
            const bool fitsPrevious = !havePrevious ||
                sessions[blockStart].startTimeMs >=
                    endTimeMs(sessions[blockStart - 1]) + restartGapMs;
            if (fitsPrevious) {
                for (size_t i = blockStart; i <= blockEnd; ++i) sessions[i].kind = Kind::Assumed;
            }
        } else if (havePrevious) {
            int64_t cursor = endTimeMs(sessions[blockStart - 1]) + restartGapMs;
            for (size_t i = blockStart; i <= blockEnd; ++i) {
                sessions[i].startTimeMs = cursor;
                sessions[i].kind = Kind::Assumed;
                cursor = endTimeMs(sessions[i]) + restartGapMs;
            }
        }
        blockStart = blockEnd + 1;
    }
}

} // namespace history_time_placement
