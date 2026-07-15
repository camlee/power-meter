#include "pm1_uart_protocol.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace sensors::pm1 {
namespace {

constexpr size_t kFieldCount = 13;

bool parseUnsigned32(const char* text, uint32_t& value) {
    if (!text || !*text) return false;
    uint64_t parsed = 0;
    for (const char* cursor = text; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        parsed = parsed * 10U + static_cast<unsigned>(*cursor - '0');
        if (parsed > std::numeric_limits<uint32_t>::max()) return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

int uppercaseHexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parseFixedHex(const char* text, size_t digits, uint32_t& value) {
    if (!text || std::strlen(text) != digits) return false;
    uint32_t parsed = 0;
    for (size_t i = 0; i < digits; ++i) {
        const int digit = uppercaseHexDigit(text[i]);
        if (digit < 0) return false;
        parsed = (parsed << 4U) | static_cast<uint32_t>(digit);
    }
    value = parsed;
    return true;
}

bool hasDecimalSyntax(const char* text) {
    if (!text || !*text) return false;
    const char* cursor = text;
    if (*cursor == '+' || *cursor == '-') ++cursor;
    bool sawDigit = false;
    bool sawPoint = false;
    for (; *cursor; ++cursor) {
        if (*cursor >= '0' && *cursor <= '9') {
            sawDigit = true;
            continue;
        }
        if (*cursor == '.' && !sawPoint) {
            sawPoint = true;
            continue;
        }
        return false;
    }
    return sawDigit;
}

bool parseFiniteDecimal(const char* text, float& value) {
    if (!hasDecimalSyntax(text)) return false;
    errno = 0;
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

bool isCounterWrap(uint32_t previous, uint32_t current) {
    return current < previous && (previous - current) > 0x80000000U;
}

bool isUptimeReset(uint32_t previous, uint32_t current) {
    if (current >= previous) return false;
    return !isCounterWrap(previous, current);
}

} // namespace

uint16_t crc16CcittFalse(const char* bytes, size_t length) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(static_cast<uint8_t>(bytes[i])) << 8U;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                                  : static_cast<uint16_t>(crc << 1U);
        }
    }
    return crc;
}

void Receiver::feed(const uint8_t* bytes, size_t length, uint32_t receiveMs) {
    if (!bytes) return;
    for (size_t i = 0; i < length; ++i) feed(static_cast<char>(bytes[i]), receiveMs);
}

void Receiver::feed(char byte, uint32_t receiveMs) {
    if (discardingOverflow_) {
        if (byte == '\n') {
            discardingOverflow_ = false;
            lineLength_ = 0;
            ++diagnostics_.overflowFrames;
            reject(ParseError::Overflow);
        }
        return;
    }

    if (byte == '\n') {
        if (lineLength_ > 0 && line_[lineLength_ - 1] == '\r') --lineLength_;
        line_[lineLength_] = '\0';
        if (lineLength_ == 0) reject(ParseError::Empty);
        else parseLine(receiveMs);
        lineLength_ = 0;
        return;
    }

    // A 159-byte payload plus LF is the largest valid 160-byte record. The
    // array's last byte is reserved for the temporary NUL terminator.
    if (lineLength_ >= kMaximumRecordBytes - 1) {
        lineLength_ = 0;
        discardingOverflow_ = true;
        return;
    }
    line_[lineLength_++] = byte;
}

void Receiver::reject(ParseError error) {
    ++diagnostics_.invalidFrames;
    diagnostics_.lastError = error;
    if (error == ParseError::Checksum) ++diagnostics_.checksumErrors;
}

bool Receiver::parseLine(uint32_t receiveMs) {
    char* star = std::strchr(line_, '*');
    if (!star || star == line_ || std::strchr(star + 1, '*')) {
        reject(ParseError::Framing);
        return false;
    }

    uint32_t suppliedCrc = 0;
    if (!parseFixedHex(star + 1, 4, suppliedCrc)) {
        reject(ParseError::Framing);
        return false;
    }
    const uint16_t calculatedCrc = crc16CcittFalse(line_, static_cast<size_t>(star - line_));
    if (calculatedCrc != suppliedCrc) {
        reject(ParseError::Checksum);
        return false;
    }
    *star = '\0';

    char* fields[kFieldCount] = {};
    size_t fieldCount = 1;
    fields[0] = line_;
    for (char* cursor = line_; *cursor; ++cursor) {
        if (*cursor != ',') continue;
        *cursor = '\0';
        if (fieldCount >= kFieldCount) {
            reject(ParseError::FieldCount);
            return false;
        }
        fields[fieldCount++] = cursor + 1;
    }
    if (fieldCount != kFieldCount) {
        reject(ParseError::FieldCount);
        return false;
    }
    if (std::strcmp(fields[0], "PM1") != 0) {
        reject(ParseError::Magic);
        return false;
    }

    uint32_t sequence = 0;
    uint32_t sourceUptimeMs = 0;
    if (!parseUnsigned32(fields[1], sequence)) {
        reject(ParseError::Sequence);
        return false;
    }
    if (!parseUnsigned32(fields[2], sourceUptimeMs)) {
        reject(ParseError::Uptime);
        return false;
    }

    uint32_t parsedMask = 0;
    if (!parseFixedHex(fields[3], 2, parsedMask) || (parsedMask & ~kSupportedChannelMask) != 0) {
        reject(ParseError::Mask);
        return false;
    }

    Channel parsedChannels[3];
    for (uint8_t channel = 0; channel < 3; ++channel) {
        const bool present = (parsedMask & (1U << channel)) != 0;
        const char* voltage = fields[4 + channel * 3];
        const char* current = fields[5 + channel * 3];
        const char* duty = fields[6 + channel * 3];
        if (!present) {
            if (*voltage || *current || *duty) {
                reject(ParseError::ChannelFields);
                return false;
            }
            continue;
        }
        if (!*voltage || !*current) {
            reject(ParseError::ChannelFields);
            return false;
        }
        if (!parseFiniteDecimal(voltage, parsedChannels[channel].voltage) ||
            !parseFiniteDecimal(current, parsedChannels[channel].current)) {
            reject(ParseError::Numeric);
            return false;
        }
        if (*duty) {
            if (!parseFiniteDecimal(duty, parsedChannels[channel].dutyCycle)) {
                reject(ParseError::Numeric);
                return false;
            }
            parsedChannels[channel].hasDutyCycle = true;
        }
    }

    if (diagnostics_.hasValidFrame && sequence == diagnostics_.sequence) {
        ++diagnostics_.duplicateFrames;
        diagnostics_.lastError = ParseError::None;
        return true;
    }

    acceptSequence(sequence, sourceUptimeMs);
    for (uint8_t channel = 0; channel < 3; ++channel) channels_[channel] = parsedChannels[channel];
    ++diagnostics_.validFrames;
    diagnostics_.lastError = ParseError::None;
    diagnostics_.hasValidFrame = true;
    diagnostics_.mask = static_cast<uint8_t>(parsedMask);
    diagnostics_.sequence = sequence;
    diagnostics_.sourceUptimeMs = sourceUptimeMs;
    diagnostics_.lastValidReceiveMs = receiveMs;
    return true;
}

void Receiver::acceptSequence(uint32_t sequence, uint32_t sourceUptimeMs) {
    if (!diagnostics_.hasValidFrame) return;
    const uint32_t previousSequence = diagnostics_.sequence;
    const uint32_t previousUptime = diagnostics_.sourceUptimeMs;
    const bool reset = isUptimeReset(previousUptime, sourceUptimeMs) ||
                       (sequence < previousSequence && !isCounterWrap(previousSequence, sequence));
    if (reset) {
        ++diagnostics_.sequenceResets;
        return;
    }
    const uint32_t delta = sequence - previousSequence;
    if (sequence < previousSequence) ++diagnostics_.sequenceWraps;
    if (delta > 1U) {
        ++diagnostics_.sequenceGapEvents;
        diagnostics_.missingFrames += delta - 1U;
    }
}

SensorSample Receiver::sample(uint8_t channel, uint32_t nowMs) const {
    SensorSample sample;
    if (channel >= 3) {
        sample.state = SensorSampleState::Invalid;
        return sample;
    }
    if (!diagnostics_.hasValidFrame) return sample;
    if ((diagnostics_.mask & (1U << channel)) == 0) {
        sample.state = SensorSampleState::NotConfigured;
        return sample;
    }
    sample.configured = true;
    if (lastValidAgeMs(nowMs) >= kStaleAfterMs) {
        sample.state = SensorSampleState::Stale;
        return sample;
    }
    const Channel& value = channels_[channel];
    sample.state = SensorSampleState::Observed;
    sample.voltage = value.voltage;
    sample.current = value.current;
    sample.hasDutyCycle = value.hasDutyCycle;
    sample.dutyCycle = value.dutyCycle;
    return sample;
}

uint32_t Receiver::lastValidAgeMs(uint32_t nowMs) const {
    if (!diagnostics_.hasValidFrame) return 0;
    return nowMs - diagnostics_.lastValidReceiveMs;
}

const char* parseErrorLabel(ParseError error) {
    switch (error) {
        case ParseError::None: return "none";
        case ParseError::Empty: return "empty";
        case ParseError::Overflow: return "overflow";
        case ParseError::Framing: return "framing";
        case ParseError::Checksum: return "checksum";
        case ParseError::FieldCount: return "field_count";
        case ParseError::Magic: return "magic";
        case ParseError::Sequence: return "sequence";
        case ParseError::Uptime: return "uptime";
        case ParseError::Mask: return "mask";
        case ParseError::ChannelFields: return "channel_fields";
        case ParseError::Numeric: return "numeric";
    }
    return "unknown";
}

} // namespace sensors::pm1
