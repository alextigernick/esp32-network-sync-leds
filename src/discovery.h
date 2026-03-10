#pragma once
#include <stdint.h>

#define MAX_PEERS 16
#define PEER_NAME_LEN 32

typedef struct {
    char ip[16];
    char name[PEER_NAME_LEN];
    uint32_t last_seen_ms;  // xTaskGetTickCount() * portTICK_PERIOD_MS
    uint32_t uptime_ms;     // peer's esp_timer_get_time() / 1000 at announce time
} peer_t;

// Start the discovery task (announce self + listen for peers)
void discovery_start(const char *my_ip, const char *my_name);

// Copy current peer list into out[]. Returns count.
int discovery_get_peers(peer_t *out, int max);

// Stack high-water-marks (words remaining) for the two discovery tasks.
uint32_t discovery_get_listen_stack_hwm(void);
uint32_t discovery_get_announce_stack_hwm(void);
