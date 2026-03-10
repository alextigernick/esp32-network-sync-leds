#pragma once

#include <stdint.h>

// Load persistent node config from NVS.  Call once after nvs_flash_init().
void     node_config_load(void);

// Current strip length (default: NUM_LEDS from config.h).
uint16_t node_config_get_num_leds(void);

// Persist a new strip length.  Clamped to [1, NUM_LEDS].  Takes effect immediately.
void     node_config_save_num_leds(uint16_t n);

// Per-device brightness ceiling (0=off, 255=full, default 255).
// The requested brightness is remapped into [0, max_bright].
uint8_t  node_config_get_max_bright(void);
void     node_config_save_max_bright(uint8_t v);

// Per-device color temperature bias (-100=cool, 0=neutral, +100=warm).
// Positive values dim the blue channel; negative values dim the red channel.
int8_t   node_config_get_ct_bias(void);
void     node_config_save_ct_bias(int8_t v);
