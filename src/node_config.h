#pragma once

#include <stdint.h>

// Load persistent node config from NVS.  Call once after nvs_flash_init().
void     node_config_load(void);

// Current strip length (default: NUM_LEDS from config.h).
uint16_t node_config_get_num_leds(void);

// Persist a new strip length.  Clamped to [1, NUM_LEDS].  Takes effect immediately.
void     node_config_save_num_leds(uint16_t n);
