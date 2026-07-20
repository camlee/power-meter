#include "http_utils.h"

#include <cctype>
#include <climits>
#include <cstring>

#include "ota_public_key.h"

#ifndef OTA_SHARED_TOKEN
#define OTA_SHARED_TOKEN ""
#endif

namespace http_utils {
namespace {

bool constantTimeEqual(const String& supplied, const char* expected) {
    const size_t expectedLength = strlen(expected);
    if (supplied.length() != expectedLength) return false;
    uint8_t different = 0;
    for (size_t i = 0; i < expectedLength; ++i) different |= supplied[i] ^ expected[i];
    return different == 0;
}

} // namespace

bool authorised(WebServer& server) {
    const char* token = OTA_SHARED_TOKEN;
    const String header = server.header("Authorization");
    constexpr char prefix[] = "Bearer ";
    return token[0] != '\0' && header.startsWith(prefix) &&
           constantTimeEqual(header.substring(sizeof(prefix) - 1), token);
}

bool jsonString(const String& json, const char* name, String& value) {
    const String key = String("\"") + name + "\"";
    const int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    const int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    const int firstQuote = json.indexOf('\"', colon + 1);
    if (firstQuote < 0) return false;
    const int lastQuote = json.indexOf('\"', firstQuote + 1);
    if (lastQuote < 0) return false;
    value = json.substring(firstQuote + 1, lastQuote);
    return true;
}

bool validSha256(const String& hash) {
    if (hash.length() != 64) return false;
    for (size_t i = 0; i < hash.length(); ++i) {
        if (!isxdigit(static_cast<unsigned char>(hash[i]))) return false;
    }
    return true;
}

bool jsonUnsigned(const String& json, const char* name, uint32_t& value) {
    const String key = String("\"") + name + "\"";
    const int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    const int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    int start = colon + 1;
    while (start < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[start]))) ++start;
    int end = start;
    while (end < static_cast<int>(json.length()) &&
           isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (end == start || (end - start) > 10) return false;
    value = 0;
    for (int i = start; i < end; ++i) value = value * 10 + (json[i] - '0');
    return true;
}

bool jsonInteger64(const String& json, const char* name, int64_t& value) {
    const String key = String("\"") + name + "\"";
    const int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    const int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;

    int cursor = colon + 1;
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    bool negative = false;
    if (cursor < static_cast<int>(json.length()) && json[cursor] == '-') {
        negative = true;
        ++cursor;
    }
    const int firstDigit = cursor;
    uint64_t magnitude = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63) : INT64_MAX;
    while (cursor < static_cast<int>(json.length()) &&
           isdigit(static_cast<unsigned char>(json[cursor]))) {
        const uint8_t digit = json[cursor++] - '0';
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (cursor == firstDigit) return false;
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    if (cursor < static_cast<int>(json.length()) && json[cursor] != ',' && json[cursor] != '}') return false;

    value = negative
        ? (magnitude == (uint64_t{1} << 63) ? INT64_MIN : -static_cast<int64_t>(magnitude))
        : static_cast<int64_t>(magnitude);
    return true;
}

bool jsonBool(const String& json, const char* name, bool& value) {
    const String key = String("\"") + name + "\"";
    const int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    const int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    int valueAt = colon + 1;
    while (valueAt < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[valueAt]))) ++valueAt;
    const String tail = json.substring(valueAt);
    if (tail.startsWith("true")) { value = true; return true; }
    if (tail.startsWith("false")) { value = false; return true; }
    return false;
}

void putLe16(uint8_t* dest, uint16_t value) {
    dest[0] = value & 0xff;
    dest[1] = value >> 8;
}

void putLe32(uint8_t* dest, uint32_t value) {
    dest[0] = value & 0xff;
    dest[1] = (value >> 8) & 0xff;
    dest[2] = (value >> 16) & 0xff;
    dest[3] = (value >> 24) & 0xff;
}

bool writeClient(WiFiClient& client, const uint8_t* data, size_t length) {
    constexpr size_t kChunkBytes = 1460;
    constexpr uint32_t kStallTimeoutMs = 1500;
    uint32_t lastProgress = millis();
    while (length > 0 && client.connected()) {
        const size_t requested = min(length, kChunkBytes);
        const size_t written = client.write(data, requested);
        if (written > 0) {
            data += written;
            length -= written;
            lastProgress = millis();
        } else if (millis() - lastProgress >= kStallTimeoutMs) {
            client.stop();
            return false;
        }
        delay(0);
    }
    return length == 0;
}

} // namespace http_utils
