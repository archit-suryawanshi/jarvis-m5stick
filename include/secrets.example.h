// Copy this file to secrets.h in the same folder and fill in your values.
// secrets.h is compiled into the firmware, so anyone holding the stick could
// read these values.
#pragma once

// Wi-Fi networks Jarvis can join. It picks the strongest one in range.
// Networks with an empty password are skipped; WIFI_OPEN marks one with no
// password. 2.4 GHz only: the ESP32 can't
// join 5 GHz networks (an iPhone hotspot works with Maximize Compatibility on).
#define WIFI_OPEN nullptr
static const struct {
  const char *ssid;
  const char *pass;
} WIFI_NETWORKS[] = {
  {"your-wifi-name", ""},
};

// Free, no card needed: https://console.groq.com/keys
#define GROQ_API_KEY "gsk_..."

// Web search. Free, no card needed (1,000 searches a month):
// https://app.tavily.com/home
#define TAVILY_API_KEY "tvly-..."

// Your time zone as a POSIX TZ string, for the clock and for questions like
// "what's on today". Examples: "IST-5:30" (India), "GMT0BST,M3.5.0/1,M10.5.0"
// (UK), "EST5EDT,M3.2.0,M11.1.0" (US Eastern), "PST8PDT,M3.2.0,M11.1.0"
// (US Pacific).
#define TIMEZONE "UTC0"
