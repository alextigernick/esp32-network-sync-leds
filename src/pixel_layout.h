#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float x;
    float y;
} pixel_pos_t;

// Parse /spiffs/pixel_layout.csv into memory. Call after SPIFFS is mounted.
void pixel_layout_load(void);

// Look up position for LED index. Returns false if not defined.
bool pixel_layout_get(int idx, float *x, float *y);

// Number of positions loaded (highest defined index + 1).
int pixel_layout_count(void);

// Write raw CSV bytes to SPIFFS and reload in-memory state.
esp_err_t pixel_layout_save_csv(const char *data, size_t len);
