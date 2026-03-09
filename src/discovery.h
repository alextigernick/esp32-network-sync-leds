#pragma once
#include <stdint.h>

#define MAX_PEERS 16
#define PEER_NAME_LEN 32

typedef struct {
    char ip[16];
    char name[PEER_NAME_LEN];
    uint32_t last_seen_ms;  // xTaskGetTickCount() * portTICK_PERIOD_MS
} peer_t;

// Start the discovery task (announce self + listen for peers)
void discovery_start(const char *my_ip, const char *my_name);

// Copy current peer list into out[]. Returns count.
int discovery_get_peers(peer_t *out, int max);
