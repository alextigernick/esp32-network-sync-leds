#pragma once

#include <stdint.h>
#include "config.h"

// Per-strip configuration (GPIO pin and LED count).
// gpio == 255 means the strip is disabled.
typedef struct {
    uint8_t  gpio;
    uint16_t num_leds;
} strip_cfg_t;

// Load persistent node config from NVS.  Call once after nvs_flash_init().
void     node_config_load(void);

// Strip configuration (up to MAX_STRIPS entries).
// Disabled strips have gpio == 255.
void     node_config_get_strips(strip_cfg_t *out, int *count_out);
void     node_config_save_strip(int i, uint8_t gpio, uint16_t num_leds);

// Total LED count across all enabled strips (convenience for flash_task).
uint16_t node_config_get_num_leds(void);

// Per-device brightness ceiling (0=off, 255=full, default 255).
uint8_t  node_config_get_max_bright(void);
void     node_config_save_max_bright(uint8_t v);

// Per-device color temperature bias (-100=cool, 0=neutral, +100=warm).
int8_t   node_config_get_ct_bias(void);
void     node_config_save_ct_bias(int8_t v);

// Per-device pixel layout transform.
// x/y offsets in mm, rotation in degrees.
// Applied at load time: each pixel is translated then rotated around the origin.
float    node_config_get_layout_x_offset(void);   // mm
float    node_config_get_layout_y_offset(void);   // mm
float    node_config_get_layout_rotation(void);   // degrees
void     node_config_save_layout_transform(float x_mm, float y_mm, float rot_deg);
