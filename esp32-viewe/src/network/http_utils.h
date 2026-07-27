#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

namespace http_utils {

bool jsonString(const String& json, const char* name, String& value);
bool jsonUnsigned(const String& json, const char* name, uint32_t& value);
bool jsonInteger64(const String& json, const char* name, int64_t& value);
bool jsonFloat(const String& json, const char* name, float& value);
bool jsonBool(const String& json, const char* name, bool& value);
// Appends a complete quoted JSON string, escaping control characters and the
// characters that are significant inside a JSON string literal.
void appendJsonString(String& json, const char* value);
bool validSha256(const String& hash);

void putLe16(uint8_t* dest, uint16_t value);
void putLe32(uint8_t* dest, uint32_t value);

// Makes bounded progress on a synchronous response and abandons a client that
// stops accepting bytes. Suitable for generated binary responses of any kind.
bool writeClient(WiFiClient& client, const uint8_t* data, size_t length);

} // namespace http_utils
