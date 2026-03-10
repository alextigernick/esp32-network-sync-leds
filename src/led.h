#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialize all WS2812B strips from node_config via RMT.
// Call once before any led_set* calls.
void led_init(void);

// Re-initialize strips after node_config strip settings have changed.
// Safe to call at runtime; waits for any in-progress transmit to finish.
void led_reinit(void);

// Set all LEDs on all strips to the given RGB color.
void led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

// Turn all LEDs on (using last set color) or off.
void led_set(bool on);

// Light a single pixel by flat strip index using the current color; clears all others.
// Pass idx = -1 to just clear all strips.
void led_set_pixel(int idx);

// Write arbitrary per-pixel RGB data. rgb must point to count*3 bytes in R,G,B order.
// Pixels are distributed across strips sequentially (strip 0 first, then strip 1, ...).
// Overrides the uniform color; disables probe mode.
void led_write_rgb(const uint8_t *rgb, int count);
