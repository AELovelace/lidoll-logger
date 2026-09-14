#pragma once
// Copy to config.h and fill these values. Keep config.h and configured firmware private.
// Optional first-boot fallback. Leave blank to choose and save a network from the device's Wi-Fi menu.
static constexpr char WIFI_SSID[] = "";
static constexpr char WIFI_PASSWORD[] = "";
static constexpr char STATS_API_BASE[] = "http://10.1.1.23:4173/tracker/api/statistics/v1/";
static constexpr char STATS_TOKEN[] = ""; // Admin console > Statistics devices > Everyone and individual views.
static constexpr uint32_t POLL_INTERVAL_MS = 60000;
// For https:// URLs, paste the server's trusted root CA PEM here. TLS validation is never disabled.
static constexpr char TLS_ROOT_CA[] = R"PEM()PEM";
