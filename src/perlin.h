#pragma once
#include <stdint.h>

// Sample fractal Brownian motion (fBm) Perlin noise, returning [0, 255].
//   x_mm, y_mm  — pixel position in millimetres (from pixel_layout)
//   time_s      — synchronized time in seconds (from time_sync_get_ms)
//   scale_mm    — spatial scale: base noise feature size in mm
//   speed       — temporal drift rate in noise-units per second
//   octaves     — number of fBm octaves (1–8); output is normalised by
//                 the geometric amplitude sum so range is consistent
//                 across all nodes for any octave count
uint8_t perlin_sample(float x_mm, float y_mm, float time_s,
                      float scale_mm, float speed, int octaves);
