#include "http_utils.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace http_utils {

bool jsonString(const String& json, const char* name, String& value) {
    const String key = String("\"") + name + "\"";
    const int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    const int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    int cursor = colon + 1;
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    if (cursor >= static_cast<int>(json.length()) || json[cursor++] != '\"') return false;

    value = "";
    while (cursor < static_cast<int>(json.length())) {
        const char ch = json[cursor++];
        if (ch == '\"') return true;
        if (static_cast<unsigned char>(ch) < 0x20) return false;
        if (ch != '\\') {
            value += ch;
            continue;
        }
        if (cursor >= static_cast<int>(json.length())) return false;
        const char escaped = json[cursor++];
        switch (escaped) {
            case '\"': value += '\"'; break;
            case '\\': value += '\\'; break;
            case '/': value += '/'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            // JSON.stringify emits non-ASCII text as UTF-8, so the only \u
            // escapes it normally sends here are control characters. Decode
            // that byte range and reject larger escapes rather than silently
            // changing a Wi-Fi credential.
            case 'u': {
                if (cursor + 4 > static_cast<int>(json.length())) return false;
                uint16_t codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const char hex = json[cursor++];
                    codepoint <<= 4;
                    if (hex >= '0' && hex <= '9') codepoint |= hex - '0';
                    else if (hex >= 'a' && hex <= 'f') codepoint |= hex - 'a' + 10;
                    else if (hex >= 'A' && hex <= 'F') codepoint |= hex - 'A' + 10;
                    else return false;
                }
                if (codepoint > 0x7f) return false;
                value += static_cast<char>(codepoint);
                break;
            }
            default: return false;
        }
    }
    return false;
}

void appendJsonString(String& json, const char* value) {
    static constexpr char hex[] = "0123456789abcdef";
    json += '\"';
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(value ? value : "");
         *cursor; ++cursor) {
        switch (*cursor) {
            case '\"': json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\b': json += "\\b"; break;
            case '\f': json += "\\f"; break;
            case '\n': json += "\\n"; break;
            case '\r': json += "\\r"; break;
            case '\t': json += "\\t"; break;
            default:
                if (*cursor < 0x20) {
                    json += "\\u00";
                    json += hex[*cursor >> 4];
                    json += hex[*cursor & 0x0f];
                } else {
                    json += static_cast<char>(*cursor);
                }
                break;
        }
    }
    json += '\"';
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

bool jsonFloat(const String& json, const char* name, float& value) {
    const String key = String("\"") + name + "\"";
    const int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    const int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    int start = colon + 1;
    while (start < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[start]))) ++start;
    if (start >= static_cast<int>(json.length())) return false;
    char* end = nullptr;
    const float parsed = strtof(json.c_str() + start, &end);
    if (end == json.c_str() + start || !std::isfinite(parsed)) return false;
    while (*end && isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end && *end != ',' && *end != '}') return false;
    value = parsed;
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
