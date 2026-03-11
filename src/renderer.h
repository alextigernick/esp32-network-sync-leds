#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// LED pattern renderer — 50 Hz render loop driving WS2812B strips.
//
// Reads synchronized settings (via settings_get()) and the synced clock
// (via time_sync_get_ms()) each frame, evaluating sine waves or Perlin noise
// per pixel, applying palette lookup, color-temperature correction, and writing
// the result to the strip driver.
// ---------------------------------------------------------------------------

// Initialize the LED driver and start the render task.
// Must be called after node_config_load() and pixel_layout_load().
// settings_sync_init() must be called before this.
void renderer_start(void);

// Blank all LEDs and pause rendering for the duration of an OTA flash.
// Call with enable=true before writing firmware, enable=false after reboot
// (the reboot itself ends the blackout, but this is here for completeness).
void renderer_set_ota_blackout(bool enable);

// Flash all LEDs on this node solid white for duration_ms milliseconds.
// Used to visually identify a node. Local only — not forwarded to peers.
void renderer_identify(uint32_t duration_ms);

// Diagnostic counters from the render task.
uint32_t renderer_get_frame_count(void);
uint32_t renderer_get_stack_hwm(void); // FreeRTOS stack words remaining
