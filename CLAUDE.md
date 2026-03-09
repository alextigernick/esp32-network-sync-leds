# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
# Build
pio run -e seeed_xiao_esp32c3

# Flash
pio run -e seeed_xiao_esp32c3 --target upload

# Monitor serial (115200 baud, USB Serial/JTAG on C3)
pio device monitor -e seeed_xiao_esp32c3

# Build + flash + monitor in one
pio run -e seeed_xiao_esp32c3 --target upload && pio device monitor -e seeed_xiao_esp32c3
```

There are no tests. The framework is **ESP-IDF** (not Arduino), accessed via PlatformIO. The env is `seeed_xiao_esp32c3`.

## Hardware

- **Board**: Seeed XIAO ESP32C3 (ESP32-C3, single-core RISC-V, 4MB flash)
- **USB**: CH340C USB-UART bridge → console on UART0 at 115200
- **LED**: External LED on GPIO 20 (no built-in user LED)
- **Strapping/USB pins to avoid**: GPIO 18/19 (USB D-/D+)

## Architecture

All nodes run identical firmware. On boot, each node attempts to join the WiFi network (`WIFI_SSID`). If connection fails after `WIFI_MAX_CONNECT_ATTEMPTS` retries, that node promotes itself to AP (`WIFI_MODE_AP`) and becomes the network host. All subsequent nodes join as STAs.

**Node roles are determined at runtime** — no compile-time flags. The AP node is the time sync master; STA nodes are slaves.

### Source files

| File | Responsibility |
|------|---------------|
| `src/main.c` | Boot, WiFi init (connect-or-become-AP logic), task orchestration |
| `src/config.h` | All tunables: SSID, password, GPIO, ports, timeouts |
| `src/discovery.c/h` | UDP multicast peer discovery (`239.0.0.1:5000`). Two tasks: announce (sends own IP/name every 3s) and listen (receives peers, expires stale ones) |
| `src/time_sync.c/h` | UDP time sync on port 5001. Master serves current ms timestamp on request; slaves poll every 5s and apply RTT-corrected offset to `esp_timer_get_time()` |
| `src/web_server.c/h` | ESP-IDF `esp_http_server`. GET `/` serves control page; POST `/led` toggles GPIO 20; POST `/ota` accepts multipart firmware upload and reboots. GET/POST `/settings` for synchronized settings. GET `/state` returns JSON with full state including mode, pattern params, palette, and peers. |
| `src/settings_sync.c/h` | Synchronized settings. `settings_t` holds all shared state; `settings_apply_and_forward()` applies locally then pushes to all peers via HTTP POST `/settings?fwd=0`. `flash_task` runs at 10 ms resolution driving WS2812B strip for all modes. |
| `src/led.c/h` | WS2812B strip driver via RMT. `led_set(bool)` uniform color, `led_set_pixel(int)` probe mode, `led_write_rgb(const uint8_t*, int)` per-pixel RGB buffer. |
| `src/pixel_layout.c/h` | Per-node pixel position map loaded from `/spiffs/pixel_layout.csv` (index, x_mm, y_mm). `pixel_layout_get(i, &x, &y)` used by pattern renderer. |
| `src/perlin.c/h` | Pure-C fixed-point Perlin noise (16.16 format). `perlin_sample(x_mm, y_mm, time_s, scale_mm, speed, octaves)` returns [0,255] via fBm with normalization by geometric amplitude sum. |

### Key design decisions

- **AP IP is fixed at `192.168.4.1`** (lwIP default for AP mode). STA nodes hard-code this as the time sync master address.
- **Discovery uses multicast**, not broadcast. Works reliably when the AP is one of our ESP32s (lwIP forwards multicast between STAs). May fail on external routers with AP isolation.
- **All tasks use `SO_RCVTIMEO` + `vTaskDelay`** on error paths to prevent tight loops that starve the FreeRTOS idle task (causing `TG1WDT_SYS_RST` watchdog resets).
- **Task priorities are 3** — below WiFi/lwIP internals (~19–22) so networking stack always preempts our tasks.
- Node name is derived from last 3 bytes of STA MAC address (e.g. `node-C9604C`), read via `esp_wifi_get_mac` after WiFi init.
