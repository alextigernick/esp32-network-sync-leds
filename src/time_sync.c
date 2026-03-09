#include "time_sync.h"
#include "config.h"
#include "discovery.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "time_sync"

// Protocol: slave sends "REQ\n", master replies with 8-byte big-endian uint64 (ms since master boot)

// Offset applied to esp_timer_get_time() so all nodes share a common epoch.
// On master: offset = 0 (master IS the reference).
// On slaves: offset = master_time - local_time_at_sync.
static int64_t  s_offset_us   = 0;
static int64_t  s_last_rtt_us = -1;
static uint32_t s_sync_count  = 0;
static uint32_t s_fail_count  = 0;
static char     s_role[20]    = "master";

void time_sync_get_debug(time_sync_debug_t *out) {
    out->offset_us   = s_offset_us;
    out->last_rtt_us = s_last_rtt_us;
    out->sync_count  = s_sync_count;
    out->fail_count  = s_fail_count;
    strncpy(out->role, s_role, sizeof(out->role) - 1);
    out->role[sizeof(out->role) - 1] = '\0';
}

uint64_t time_sync_get_ms(void) {
    int64_t local_us = (int64_t)esp_timer_get_time();
    return (uint64_t)((local_us + s_offset_us) / 1000);
}

// ---- master ------------------------------------------------------------

static void master_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "master socket failed"); vTaskDelete(NULL); return; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TIME_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    char buf[16];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Master time server running");

    while (1) {
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&client, &clen);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        buf[n] = '\0';
        if (strncmp(buf, "REQ", 3) != 0) continue;

        uint64_t t = time_sync_get_ms();
        // Send as 8 bytes big-endian
        uint8_t resp[8];
        resp[0] = (t >> 56) & 0xFF;
        resp[1] = (t >> 48) & 0xFF;
        resp[2] = (t >> 40) & 0xFF;
        resp[3] = (t >> 32) & 0xFF;
        resp[4] = (t >> 24) & 0xFF;
        resp[5] = (t >> 16) & 0xFF;
        resp[6] = (t >>  8) & 0xFF;
        resp[7] = (t      ) & 0xFF;
        sendto(sock, resp, 8, 0, (struct sockaddr *)&client, clen);
    }
}

static void start_master_task(void) {
    xTaskCreate(master_task, "time_master", 6144, NULL, 3, NULL);
}

void time_sync_start_master(void) {
    strncpy(s_role, "master", sizeof(s_role) - 1);
    start_master_task();
}

// ---- slave -------------------------------------------------------------

static char s_master_ip[16];

static void slave_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "slave socket failed"); vTaskDelete(NULL); return; }

    // Set receive timeout so we don't block forever
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in master_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(TIME_SYNC_PORT),
    };
    inet_aton(s_master_ip, &master_addr.sin_addr);

    while (1) {
        int64_t t0 = (int64_t)esp_timer_get_time();

        sendto(sock, "REQ\n", 4, 0, (struct sockaddr *)&master_addr, sizeof(master_addr));

        uint8_t resp[8];
        int n = recv(sock, resp, 8, 0);

        if (n == 8) {
            int64_t t1 = (int64_t)esp_timer_get_time();
            int64_t rtt_us = t1 - t0;

            uint64_t master_ms =
                ((uint64_t)resp[0] << 56) | ((uint64_t)resp[1] << 48) |
                ((uint64_t)resp[2] << 40) | ((uint64_t)resp[3] << 32) |
                ((uint64_t)resp[4] << 24) | ((uint64_t)resp[5] << 16) |
                ((uint64_t)resp[6] <<  8) | ((uint64_t)resp[7]);

            // Estimate master time at the midpoint of the round trip
            int64_t master_us = (int64_t)(master_ms * 1000) + rtt_us / 2;
            int64_t local_us  = (t0 + t1) / 2;
            s_offset_us = master_us - local_us;
            s_last_rtt_us = rtt_us;
            s_sync_count++;

            ESP_LOGI(TAG, "Time synced. offset=%lld us, rtt=%lld us", (long long)s_offset_us, (long long)rtt_us);
        } else {
            s_fail_count++;
            ESP_LOGW(TAG, "Time sync request timed out");
        }

        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_INTERVAL_MS));
    }
}

void time_sync_start_slave(const char *master_ip) {
    strncpy(s_master_ip, master_ip, 15);
    strncpy(s_role, "slave", sizeof(s_role) - 1);
    xTaskCreate(slave_task, "time_slave", 6144, NULL, 3, NULL);
}

// ---- elected slave (leader election) -----------------------------------

static char s_my_ip[16];

static uint32_t ip_to_u32(const char *ip) {
    struct in_addr addr;
    inet_aton(ip, &addr);
    return ntohl(addr.s_addr);
}

// Try one sync round to the given IP. Returns true on success.
static bool try_sync(int sock, const char *master_ip) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(TIME_SYNC_PORT),
    };
    inet_aton(master_ip, &addr.sin_addr);

    int64_t t0 = (int64_t)esp_timer_get_time();
    sendto(sock, "REQ\n", 4, 0, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t resp[8];
    int n = recv(sock, resp, 8, 0);
    if (n != 8) return false;

    int64_t t1 = (int64_t)esp_timer_get_time();
    int64_t rtt_us = t1 - t0;

    uint64_t master_ms =
        ((uint64_t)resp[0] << 56) | ((uint64_t)resp[1] << 48) |
        ((uint64_t)resp[2] << 40) | ((uint64_t)resp[3] << 32) |
        ((uint64_t)resp[4] << 24) | ((uint64_t)resp[5] << 16) |
        ((uint64_t)resp[6] <<  8) | ((uint64_t)resp[7]);

    int64_t master_us = (int64_t)(master_ms * 1000) + rtt_us / 2;
    int64_t local_us  = (t0 + t1) / 2;
    s_offset_us = master_us - local_us;
    s_last_rtt_us = rtt_us;
    s_sync_count++;

    ESP_LOGI(TAG, "Synced to %s. offset=%lld us, rtt=%lld us",
             master_ip, (long long)s_offset_us, (long long)rtt_us);
    return true;
}

static void elected_slave_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "elected socket failed"); vTaskDelete(NULL); return; }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t my_u32 = ip_to_u32(s_my_ip);

    while (1) {
        // Elect: lowest IP among {discovered peers} ∪ {self}
        // (AP_IP is excluded — in elected mode the AP is not one of our nodes)
        peer_t peers[MAX_PEERS];
        int count = discovery_get_peers(peers, MAX_PEERS);

        uint32_t best_u32 = my_u32;
        char best_ip[16];
        strncpy(best_ip, s_my_ip, sizeof(best_ip));

        for (int i = 0; i < count; i++) {
            uint32_t p = ip_to_u32(peers[i].ip);
            if (p < best_u32) {
                best_u32 = p;
                strncpy(best_ip, peers[i].ip, sizeof(best_ip));
            }
        }

        if (my_u32 <= best_u32) {
            // Self is elected root — serve time, don't apply an offset
            strncpy(s_role, "root", sizeof(s_role) - 1);
            ESP_LOGI(TAG, "Elected as time root (lowest IP: %s)", s_my_ip);
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_INTERVAL_MS));
            continue;
        }

        snprintf(s_role, sizeof(s_role), "->%s", best_ip);
        // Try to sync to the elected master
        if (!try_sync(sock, best_ip)) {
            s_fail_count++;
            ESP_LOGW(TAG, "Sync to elected master %s failed", best_ip);
        }

        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_INTERVAL_MS));
    }
}

void time_sync_start_elected(const char *my_ip) {
    strncpy(s_my_ip, my_ip, sizeof(s_my_ip) - 1);
    strncpy(s_role, "elected", sizeof(s_role) - 1);
    // Run master server so peers can elect us if we win
    start_master_task();
    xTaskCreate(elected_slave_task, "time_elected", 6144, NULL, 3, NULL);
}
