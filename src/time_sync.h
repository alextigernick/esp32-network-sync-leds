#pragma once
#include <stdint.h>

// Start master role: serve time to requesting nodes
void time_sync_start_master(void);

// Start slave role: periodically request time from master_ip
void time_sync_start_slave(const char *master_ip);

// Start elected mode (for STA nodes when AP may not be an ESP32 node).
// Runs the master server AND a slave that each cycle elects the lowest-IP
// node (from AP_IP + discovered peers + self) as the time reference.
// If self is elected, no offset is applied (this node is the root).
void time_sync_start_elected(const char *my_ip);

// Return current synced time in milliseconds (monotonic, shared across nodes)
uint64_t time_sync_get_ms(void);
