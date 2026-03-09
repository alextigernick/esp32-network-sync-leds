#pragma once

// WiFi credentials — all nodes share these
#define WIFI_SSID       "meshleds"
#define WIFI_PASSWORD   "meshleds"

// AP address (fixed)
#define AP_IP           "192.168.4.1"

// External LED GPIO
#define LED_GPIO        20

// UDP multicast group and port for peer discovery
#define DISCOVERY_MCAST_ADDR  "239.0.0.1"
#define DISCOVERY_PORT        5000

// UDP port for time sync
#define TIME_SYNC_PORT  5001

// How many STA connection attempts before giving up and becoming the AP
#define WIFI_MAX_CONNECT_ATTEMPTS  2

// How often each node announces itself (ms)
#define DISCOVERY_INTERVAL_MS  3000

// How often STA nodes request time sync (ms)
#define TIME_SYNC_INTERVAL_MS  5000
