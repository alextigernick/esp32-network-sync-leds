#pragma once

// WiFi credentials — all nodes share these
#define WIFI_SSID       "meshleds"
#define WIFI_PASSWORD   "meshleds"

// AP address (fixed)
#define AP_IP           "192.168.4.1"

// WS2812B strip: data pin and length
#define LED_GPIO        20
#define NUM_LEDS        8

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
#define TIME_SYNC_INTERVAL_MS  1000

// Samples taken per sync round; best (min-RTT) is used
#define TIME_SYNC_SAMPLES      8

// Gap between samples in one sync burst (ms)
#define TIME_SYNC_SAMPLE_GAP_MS  3

// EWMA weight for new offset sample (0–256, higher = faster tracking)
// 77 ≈ 0.3 — converges in ~10 syncs, filters single-sample spikes
#define TIME_SYNC_EWMA_ALPHA   77

// HTTP timeout when forwarding settings to peers (ms)
#define SETTINGS_HTTP_TIMEOUT_MS  1500
