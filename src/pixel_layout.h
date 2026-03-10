#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int16_t x;  // position in 0.1 mm
    int16_t y;
} pixel_pos_t;

// Parse /spiffs/pixel_layout.csv into memory. Call after SPIFFS is mounted.
void pixel_layout_load(void);

// Look up position for LED index in 0.1 mm units. Returns false if not defined.
bool pixel_layout_get(int idx, int16_t *x, int16_t *y);

// Number of positions loaded (highest defined index + 1).
int pixel_layout_count(void);

// Write raw CSV bytes to SPIFFS and reload in-memory state.
esp_err_t pixel_layout_save_csv(const char *data, size_t len);
