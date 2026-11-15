#pragma once

// Fill this in and rename to secrets.h (not tracked by git)
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// "homeassistant.local" is resolved through mDNS; a fixed IP (e.g. "192.168.1.10") is the most reliable
#define HA_HOST_ADDR   "homeassistant.local"
#define HA_PORT_NUM    80

// Home Assistant -> Profile -> Security -> Long-lived access tokens
#define HA_TOKEN_VALUE "YOUR_LONG_LIVED_TOKEN"
