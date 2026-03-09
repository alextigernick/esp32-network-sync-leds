#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialize the WS2812B strip on LED_GPIO via RMT.
// Call once before any led_set* calls.
void led_init(void);

// Set all LEDs to the given RGB color.
void led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

// Turn all LEDs on (using last set color) or off.
void led_set(bool on);

// Light a single pixel by strip index using the current color; clears all others.
// Pass idx = -1 to just clear the strip.
void led_set_pixel(int idx);

// Write arbitrary per-pixel RGB data. rgb must point to count*3 bytes in R,G,B order.
// Overrides the uniform color; disables probe mode.
void led_write_rgb(const uint8_t *rgb, int count);

// Update the active LED count at runtime (clamped to [1, NUM_LEDS]).
void led_set_count(uint16_t n);
