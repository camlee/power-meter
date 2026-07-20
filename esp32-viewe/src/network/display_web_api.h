#pragma once

#include <Arduino.h>

class WebServer;

namespace display_web_api {

void registerRoutes(WebServer& server);

const char* appearanceModeName();
bool isDark();
bool isValidAppearance(const String& value);
void setAppearance(const String& value);
String lvglVersion();

} // namespace display_web_api
