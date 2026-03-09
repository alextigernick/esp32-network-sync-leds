#pragma once
#include <stdint.h>

// Start master role: serve time to requesting nodes
void time_sync_start_master(void);

// Start slave role: periodically request time from master_ip
void time_sync_start_slave(const char *master_ip);

// Return current synced time in milliseconds (monotonic, shared across nodes)
uint64_t time_sync_get_ms(void);
