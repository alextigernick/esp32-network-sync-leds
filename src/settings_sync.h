#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// All synchronized settings.  To add a new setting:
//   1. Add a field here.
//   2. Update settings_encode() and settings_decode() in settings_sync.c.
//   3. Add it to the /state JSON and the web UI.
// ---------------------------------------------------------------------------
typedef struct {
    bool     flash_enabled;
    uint32_t period_ms;    // 100 – 10 000
    uint8_t  duty_percent; // 1 – 100
    uint8_t  r, g, b;     // LED on-color (default white)
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
