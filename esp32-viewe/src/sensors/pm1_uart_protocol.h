#pragma once

#include "sensor_source.h"
#include <cstddef>
#include <cstdint>

namespace sensors::pm1 {

constexpr size_t kMaximumRecordBytes = 160;
constexpr uint32_t kStaleAfterMs = 2000;
constexpr uint8_t kSupportedChannelMask = 0x07;

enum class ParseError : uint8_t {
    None,
    Empty,
    Overflow,
    Framing,
    Checksum,
    FieldCount,
    Magic,
    Sequence,
    Uptime,
    Mask,
    ChannelFields,
    Numeric,
};

struct Diagnostics {
    uint32_t validFrames = 0;
    uint32_t invalidFrames = 0;
    uint32_t overflowFrames = 0;
    uint32_t checksumErrors = 0;
    uint32_t duplicateFrames = 0;
    uint32_t sequenceGapEvents = 0;
    uint32_t missingFrames = 0;
    uint32_t sequenceResets = 0;
    uint32_t sequenceWraps = 0;
    ParseError lastError = ParseError::None;
    bool hasValidFrame = false;
    uint8_t mask = 0;
    uint32_t sequence = 0;
    uint32_t sourceUptimeMs = 0;
    uint32_t lastValidReceiveMs = 0;
};

// Incremental, allocation-free receiver for one PM1 byte stream. feed() may
// be called with arbitrary chunks; accepted frames atomically replace all
// three channel values. Parser errors never erase the last fresh frame.
class Receiver {
public:
    void feed(const uint8_t* bytes, size_t length, uint32_t receiveMs);
    void feed(char byte, uint32_t receiveMs);

    SensorSample sample(uint8_t channel, uint32_t nowMs) const;
    Diagnostics diagnostics() const { return diagnostics_; }
    uint32_t lastValidAgeMs(uint32_t nowMs) const;

private:
    struct Channel {
        float voltage = 0.0f;
        float current = 0.0f;
        bool hasDutyCycle = false;
        float dutyCycle = 0.0f;
    };

    bool parseLine(uint32_t receiveMs);
    void reject(ParseError error);
    void acceptSequence(uint32_t sequence, uint32_t sourceUptimeMs);

    char line_[kMaximumRecordBytes] = {};
    size_t lineLength_ = 0;
    bool discardingOverflow_ = false;
    Channel channels_[3];
    Diagnostics diagnostics_;
};

uint16_t crc16CcittFalse(const char* bytes, size_t length);
const char* parseErrorLabel(ParseError error);

} // namespace sensors::pm1
