# self-sync-leds

Firmware for a network of ESP32-C3 nodes that drive WS2812B LED strips in tight synchrony. Any number of nodes run identical firmware. When powered on, the first node becomes the WiFi access point and time-sync root; all subsequent nodes join as stations and followers. Every node renders the same pattern at the same moment, so LED arrays distributed across a physical space appear as one continuous display.

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | Seeed XIAO ESP32-C3 (single-core RISC-V, 4 MB flash) |
| USB | CH340C USB-UART bridge — console on UART0 at 115200 baud |
| LEDs | WS2812B strips on any available GPIO (configured per device via NVS) |
| Pins to avoid | GPIO 18/19 (USB D−/D+) |

Up to four LED strips per node, configured independently per device.

---

## Build & Flash

Requires [PlatformIO](https://platformio.org/). Framework is **ESP-IDF** (not Arduino).

```bash
# Build
pio run -e seeed_xiao_esp32c3

# Flash
pio run -e seeed_xiao_esp32c3 --target upload

# Serial monitor (115200 baud)
pio device monitor -e seeed_xiao_esp32c3

# Build + flash + monitor in one command
pio run -e seeed_xiao_esp32c3 --target upload && pio device monitor -e seeed_xiao_esp32c3
```

All tunables (SSID, password, ports, timeouts) are in [`src/config.h`](src/config.h).

---

## How It Works

### Boot & Role Assignment

Every node boots with the same firmware and no compile-time role flags. The first thing each node does is try to join the WiFi network named `WIFI_SSID`. If the connection fails after `WIFI_MAX_CONNECT_ATTEMPTS` retries, that node promotes itself to **AP mode** and becomes the network host at the fixed IP `192.168.4.1`. All subsequent nodes join as **STA mode** clients.

Role summary:

| Role | WiFi mode | Time sync role | First action |
|------|-----------|----------------|--------------|
| AP (first node) | Access point | Time root | Apply saved default preset |
| STA (later nodes) | Station | Elected follower | Fetch settings from root on first sync |

### Time Synchronization

All pattern animations are driven by a shared clock — `time_sync_get_ms()` — which returns the same value on every node at the same real-world instant.

The AP runs a UDP time-sync server on port 5001. STA nodes use an **election protocol**: the node with the longest uptime is elected as the local time reference (stable root avoids clock drift cascades). Each follower sends a burst of 8 `REQ` packets, measures RTT for each, picks the best (lowest RTT) sample, and applies an EWMA-smoothed offset:

```
offset = root_time + rtt/2 - local_time
```

Followers re-sync every 1 second. If the elected root is the node itself, no offset is applied.

### Pattern Rendering

The renderer runs at **50 Hz** in a dedicated FreeRTOS task. Each frame:

1. **Evaluate pattern** (per pixel): Sine wave or Perlin fBm noise using the synced timestamp → `uint8_t` intensity in [0, 255]
2. **Palette lookup**: Map intensity through 1–4 color stops with configurable blending (linear, cosine, nearest, step)
3. **Color temperature correction**: Per-device warm/cool bias dims the blue or red channel
4. **Write to strip**: Push the RGB buffer via RMT

For **flash mode**, the renderer uses the synced clock to compute a phase within the configured period and turns the strip on or off with a duty-cycle.

All math is integer-only (16-bit fixed point). The sine lookup table avoids any FPU calls in the render loop.

### Settings Synchronization

`settings_t` holds all pattern parameters: mode, color, palette, sine/perlin parameters. When a node receives a settings change (via HTTP POST `/settings`), it applies the change locally and then asynchronously pushes it to all peers via HTTP POST `/settings?fwd=0`. Peers apply without re-forwarding, preventing loops.

### Peer Discovery

Nodes announce themselves via UDP multicast (`239.0.0.1:5000`) every 3 seconds with their IP, name, and uptime. Peers are considered stale after 6 seconds (triggering a direct unicast keepalive) and fully expired after 30 seconds. The peer list is used by time-sync election and settings forwarding.

---

## Web Interface

Each node runs an HTTP server. Connect to `http://192.168.4.1/` (AP node) or any node's IP.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI |
| `/state` | GET | JSON: mode, peers, time-sync diagnostics, render stats |
| `/settings` | GET | Current settings as URL-encoded form |
| `/settings` | POST | Apply settings (forwarded to all peers unless `?fwd=0`) |
| `/led` | POST | Immediate GPIO toggle: `state=on\|off` |
| `/led_pixel` | POST | Light a single LED by index for calibration: `idx=<n>` |
| `/node_config` | GET/POST | Per-device LED strip config (GPIO, count, brightness, color temp) |
| `/layout_transform` | GET/POST | Per-device pixel layout transform (X/Y offset mm, rotation °) |
| `/pixel_layout` | GET/POST | Upload/download LED position CSV (`idx,x_mm,y_mm`) |
| `/presets` | GET/POST | Named preset management |
| `/identify` | POST | Flash this node white for 3 s |
| `/ota` | POST | OTA firmware update (multipart `.bin`) |
| `/fwd/<ip><path>` | any | AP-side proxy — forwards request to peer at `<ip>` |

---

## LED Patterns

### Flash (`mode=0`)
All LEDs pulse on/off at a synchronized period and duty cycle. `r/g/b` sets the on-color.

### Sine (`mode=1`)
A traveling sine wave is projected across the physical LED positions. Parameters:
- `speriod` — spatial wavelength (0.1 mm units)
- `sangle` — wave propagation direction (0.1° units)
- `sspeed` — animation speed (0.01 Hz units)
- Palette maps the [0,255] sine value to color

### Perlin (`mode=2`)
Fractal Brownian Motion (fBm) Perlin noise evaluated at each LED's physical (x, y) position plus a time dimension. Parameters:
- `pscale` — spatial feature size (0.1 mm units)
- `pspeed` — temporal animation rate
- `poct` — fBm octave count (1–8)
- Palette maps the [0,255] noise value to color

### Palette
Used by Sine and Perlin modes. Up to 4 color stops with positions [0,255] and blending mode:
- `0` = linear
- `1` = nearest neighbor
- `2` = cosine smoothstep
- `3` = step (hard boundaries)

---

## Per-Device Configuration (NVS)

Each node stores hardware and layout config in NVS namespace `node_cfg`. These settings are local — not synchronized across nodes.

| NVS key | Type | Description |
|---------|------|-------------|
| `s0_gpio`–`s3_gpio` | uint8 | GPIO pin for strip 0–3 (255 = disabled) |
| `s0_leds`–`s3_leds` | uint16 | LED count for strip 0–3 |
| `max_bright` | uint8 | Per-device brightness ceiling (0–255) |
| `ct_bias` | int8 | Color temperature bias (−100=cool, 0=neutral, +100=warm) |
| `lay_x`, `lay_y` | float | Layout X/Y offset in mm |
| `lay_rot` | float | Layout rotation in degrees |

The layout transform is applied at boot when loading the pixel position CSV. It maps each LED's local (x, y) coordinates into the shared world coordinate space used by the renderer.

---

## Pixel Layout Calibration

For multi-node setups, each node needs a pixel layout CSV (`/spiffs/pixel_layout.csv`) mapping LED indices to physical positions, and a layout transform (offset + rotation) placing those positions in a shared world coordinate frame.

A browser-based calibration tool (`calibrate.html`) automates this:
1. Uses a webcam to observe the LED array
2. Scans each LED individually, captures its pixel position in the camera image
3. Fits the observed positions to the known LED layout geometry
4. Computes the layout transform for each node and uploads it via `/layout_transform`

---

## Source Files

| File | Responsibility |
|------|---------------|
| `src/main.c` | Boot, WiFi init (connect-or-become-AP), task orchestration |
| `src/config.h` | All tunables: SSID, ports, timeouts, hardware limits |
| `src/renderer.c/h` | 50 Hz render loop: sine/Perlin evaluation, palette lookup, color-temp correction, LED write |
| `src/settings_sync.c/h` | Synchronized settings state, encode/decode, HTTP peer forwarding |
| `src/discovery.c/h` | UDP multicast peer discovery and stale peer management |
| `src/time_sync.c/h` | UDP time sync — root server, follower polling, uptime-based election |
| `src/web_server.c/h` | ESP-IDF HTTP server: web UI, REST API, OTA, proxy |
| `src/led.c/h` | WS2812B strip driver via RMT (up to 4 strips, 2 parallel channels) |
| `src/pixel_layout.c/h` | Per-LED position map from SPIFFS CSV with per-device transform applied |
| `src/node_config.c/h` | Per-device persistent config in NVS (strips, brightness, color temp, layout transform) |
| `src/perlin.c/h` | Fixed-point 3D Perlin noise with fBm octaves |
| `src/presets.c/h` | Named preset save/load/delete via NVS, boot-time default preset |
