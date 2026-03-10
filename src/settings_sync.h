#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// All synchronized settings.  To add a new setting:
//   1. Add a field here.
//   2. Update settings_encode() and settings_decode() in settings_sync.c.
//   3. Add it to the /state JSON and the web UI.
// ---------------------------------------------------------------------------
typedef enum {
    MODE_FLASH   = 0,
    MODE_SINE    = 1,
    MODE_PERLIN  = 2,
} led_mode_t;

typedef struct {
    // Common
    uint8_t  mode;         // led_mode_t
    uint8_t  r, g, b;     // LED on-color

    // MODE_FLASH
    bool     flash_enabled;
    uint32_t period_ms;    // 100 – 10 000
    uint8_t  duty_percent; // 1 – 100

    // MODE_SINE
    uint32_t sine_period_mm10; // period in 0.1 mm units (100 = 10.0 mm)
    int32_t  sine_angle_deg10; // wave angle, 0.1° units (900 = 90.0°)
    int32_t  sine_speed_c100;  // animation speed in 0.01 Hz units (100 = 1.00 Hz)

    // MODE_PERLIN
    uint32_t perlin_scale_mm10; // spatial scale in 0.1 mm units (1000 = 100.0 mm)
    int32_t  perlin_speed_c100; // temporal drift in 0.0025 noise-units/s (400 = 1.00)
    uint8_t  perlin_octaves;    // fBm octave count, 1–8

    // Palette — used by MODE_SINE and MODE_PERLIN.
    // Pattern brightness (0–1) is interpolated across pal_n colour stops.
    // pal_bright is an overall dimmer applied after lookup (0 = off, 255 = full).
    uint32_t pal_colors[4]; // packed 0x00RRGGBB, stops 0..pal_n-1
    uint8_t  pal_pos[4];   // position of each stop along gradient, 0–255 (0=left, 255=right)
    uint8_t  pal_n;         // active stop count, 1–4
    uint8_t  pal_bright;    // overall brightness 0–255
    uint8_t  pal_blend;     // 0=linear, 1=nearest, 2=cosine, 3=step
} settings_t;

// Return a snapshot of current settings.
void settings_get(settings_t *out);

// Apply locally without forwarding (used when receiving a forwarded update).
void settings_apply_local(const settings_t *s);

// Apply locally and push asynchronously to all discovered peers.
void settings_apply_and_forward(const settings_t *s);

// Parse a URL-encoded body (key=val&…) into *out.  Returns true on success.
bool settings_decode(const char *body, settings_t *out);

// Encode settings into a URL-encoded string (NUL-terminated).
// buf must be at least 64 bytes.
void settings_encode(const settings_t *s, char *buf, int buf_size);

// Fetch settings from a peer via GET /settings and apply locally.
// Returns true on success.  Call once on STA boot before starting tasks.
bool settings_fetch_from_peer(const char *peer_ip);

// Start the LED flash task.  Call once after GPIO is configured.
void settings_start_flash_task(void);

// Briefly flash all LEDs white on this node for duration_ms (local only).
void settings_identify(uint32_t duration_ms);

// Diagnostic counters updated by the flash task.
uint32_t settings_get_frame_count(void);
uint32_t settings_get_flash_stack_hwm(void); // words remaining
uint32_t settings_get_fwd_stack_hwm(void);   // words remaining
