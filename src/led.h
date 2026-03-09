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
