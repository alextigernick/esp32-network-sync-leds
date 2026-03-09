#pragma once

#include <stdint.h>

// Grid layout config — per-node, stored in NVS, not synced across nodes.
//
// Describes how this node's LED strip maps onto an MxN rectangular grid
// with a meandering (boustrophedon) wiring pattern.
//
// origin:    which corner the first LED is in  (0=TL, 1=TR, 2=BL, 3=BR)
// row_first: 1 = snake across rows (L-R/R-L), 0 = snake down columns (T-B/B-T)

typedef struct {
    uint8_t rows;       // 1–32
    uint8_t cols;       // 1–32
    uint8_t origin;     // 0=TL, 1=TR, 2=BL, 3=BR
    uint8_t row_first;  // 1=row-major meander, 0=col-major meander
} grid_config_t;

// Load from NVS. Call once after nvs_flash_init().
void grid_config_load(void);

// Get current config (thread-safe copy).
void grid_config_get(grid_config_t *out);

// Persist and apply a new config. rows/cols clamped to [1, 32].
void grid_config_save(const grid_config_t *cfg);
