#pragma once
#include <stdint.h>

// Sample fractal Brownian motion (fBm) Perlin noise, returning [0, 255].
// All arithmetic is integer-only — no FPU required.
//
//   x_t, y_t    — pixel position in 0.1 mm units (from pixel_layout_get)
//   time_ms     — synchronized time in milliseconds (from time_sync_get_ms)
//   scale_t     — spatial scale in 0.1 mm: base noise feature size
//   speed_c100  — temporal drift: noise-units per second * 100
//                 (matches settings_t::perlin_speed_c100 directly)
//   octaves     — number of fBm octaves (1–8)
uint8_t perlin_sample(int16_t x_t, int16_t y_t, uint32_t time_ms,
                      int16_t scale_t, uint16_t speed_c100, int octaves);
