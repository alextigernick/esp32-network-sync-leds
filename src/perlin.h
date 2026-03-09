#pragma once
#include <stdint.h>

// Sample 3D Perlin noise, returning a brightness in [0, 255].
//   x_mm, y_mm  — pixel position in millimetres (from pixel_layout)
//   time_s      — synchronized time in seconds (from time_sync_get_ms)
//   scale_mm    — spatial scale: noise repeats roughly every scale_mm mm
//   speed       — temporal drift rate in noise-units per second
uint8_t perlin_sample(float x_mm, float y_mm, float time_s,
                      float scale_mm, float speed);
