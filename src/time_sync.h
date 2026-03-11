#pragma once
#include <stdbool.h>
#include <stdint.h>

// Start root role: serve time to requesting nodes
void time_sync_start_root(void);

// Start follower role: periodically request time from root_ip
void time_sync_start_follower(const char *root_ip);

// Start elected mode (for STA nodes when AP may not be an ESP32 node).
// Runs the root server AND an election task that each cycle elects the
// longest-uptime node (from discovered peers + self) as the time reference.
// If self is elected, no offset is applied (this node is the root).
void time_sync_start_elected(const char *my_ip);

// Return current synced time in milliseconds (monotonic, shared across nodes)
uint64_t time_sync_get_ms(void);

typedef struct {
    int64_t  offset_us;   // current clock offset applied to local timer
    int64_t  last_rtt_us; // RTT of last successful sync round-trip (-1 if never synced)
    uint32_t sync_count;  // total successful syncs
    uint32_t fail_count;  // total failed sync attempts
    char     role[20];    // "root", "follower", "elected", or "->x.x.x.x"
} time_sync_debug_t;

// Fill *out with current debug state
void time_sync_get_debug(time_sync_debug_t *out);

// Register a callback invoked once after the first successful time sync.
// Called with the IP of the peer that was synced to.
// Must be set before starting any sync task.
void time_sync_set_first_sync_cb(bool (*cb)(const char *peer_ip));

// Register a callback invoked once the first time this node wins the
// election and becomes the time root (elected mode only).
// Must be set before starting any sync task.
void time_sync_set_first_win_cb(void (*cb)(void));

// Stack high-water-mark of the running time sync task (words remaining).
uint32_t time_sync_get_stack_hwm(void);
