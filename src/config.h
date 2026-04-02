#pragma once

// WiFi credentials — all nodes share these, these are defaults but are changeable in the web ui
#define WIFI_SSID       "meshleds"
#define WIFI_PASSWORD   "meshleds"

// AP address (fixed)
#define AP_IP           "192.168.4.1"

// WS2812B strip: hard maximum pixel count (sets compile-time buffer size).
// Default strip config is all-disabled; configure pins via the web UI.
#define MAX_LEDS        2000  // compile-time buffer ceiling

// Maximum number of independent LED strips.
// The ESP32-C3 has 2 RMT TX channels; strips are driven in two sequential phases
// (0+1 then 2+3) with the RMT signals rerouted via the GPIO matrix between phases.
#define MAX_STRIPS      4

// UDP multicast group and port for peer discovery
#define DISCOVERY_MCAST_ADDR  "239.0.0.1"
#define DISCOVERY_PORT        5000

// UDP port for time sync
#define TIME_SYNC_PORT  5001

// Per-node STA retry budget is MAC-derived in range [MIN, MIN+SPREAD).
// The node with the fewest retries fails first and becomes the AP;
// others keep scanning and find it before exhausting their budget.
#define WIFI_RETRY_MIN        2
#define WIFI_RETRY_SPREAD     8    // budget = MIN + (mac_hash % SPREAD) → 2–9 attempts
#define WIFI_RETRY_DELAY_MIN_MS   0
#define WIFI_RETRY_DELAY_MAX_MS  1100  // random delay per retry in [MIN, MAX)

// Fallback used only if MAC read fails before WiFi init
#define WIFI_MAX_CONNECT_ATTEMPTS  (WIFI_RETRY_MIN + WIFI_RETRY_SPREAD - 1)

// How often each node announces itself (ms)
#define DISCOVERY_INTERVAL_MS  3000

// How often STA nodes request time sync (ms)
#define TIME_SYNC_INTERVAL_MS  1000

// Samples taken per sync round; best (min-RTT) is used
#define TIME_SYNC_SAMPLES      8

// Gap between samples in one sync burst (ms)
#define TIME_SYNC_SAMPLE_GAP_MS  3

// Early-exit threshold: stop sampling if RTT drops below this (us)
#define TIME_SYNC_GOOD_RTT_US  10000

// EWMA weight for new offset sample (0–256, higher = faster tracking)
// 77 ≈ 0.3 — converges in ~10 syncs, filters single-sample spikes
#define TIME_SYNC_EWMA_ALPHA   77

// HTTP timeout when forwarding settings to peers (ms)
#define SETTINGS_HTTP_TIMEOUT_MS  1500

// UDP broadcast port for settings sync
#define SETTINGS_UDP_PORT           5002
// UDP broadcast port for wifi credential updates
#define WIFI_CONFIG_UDP_PORT        5003
// How many times to repeat each broadcast (covers packet loss)
#define SETTINGS_UDP_REPEATS        3
// Delay between repeats (ms)
#define SETTINGS_UDP_REPEAT_DELAY_MS  80
