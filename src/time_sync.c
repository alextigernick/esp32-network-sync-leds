#include "time_sync.h"
#include "config.h"
#include "discovery.h"

#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "time_sync"

// ---------------------------------------------------------------------------
// Protocol: follower sends "REQ\n", root replies with 8-byte big-endian uint64
// containing the root's current time in MICROSECONDS.
// ---------------------------------------------------------------------------

static int64_t  s_offset_us   = 0;   // applied to local esp_timer_get_time()
static int64_t  s_last_rtt_us = -1;
static uint32_t s_sync_count  = 0;
static uint32_t s_fail_count  = 0;
static char     s_role[20]    = "root";
static bool     s_first_sync  = true;
static TaskHandle_t s_task_handle = NULL;

static bool (*s_first_sync_cb)(const char *peer_ip) = NULL;
static void (*s_first_win_cb)(void) = NULL;
static bool s_first_win = true;

void time_sync_set_first_sync_cb(bool (*cb)(const char *peer_ip)) {
    s_first_sync_cb = cb;
}

void time_sync_set_first_win_cb(void (*cb)(void)) {
    s_first_win_cb = cb;
}

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

// Encode a uint64 µs value into 8 big-endian bytes
static void encode_us(uint64_t t, uint8_t *out) {
    for (int i = 7; i >= 0; i--) { out[i] = t & 0xFF; t >>= 8; }
}

// Decode 8 big-endian bytes to uint64 µs
static uint64_t decode_us(const uint8_t *b) {
    uint64_t t = 0;
    for (int i = 0; i < 8; i++) t = (t << 8) | b[i];
    return t;
}

// Apply a new best-RTT offset sample via EWMA (or directly on first sync)
static void apply_offset(int64_t raw_offset, int64_t rtt_us) {
    if (s_first_sync) {
        s_offset_us  = raw_offset;
        s_first_sync = false;
    } else {
        // Fixed-point EWMA: new = alpha*raw + (1-alpha)*old,  alpha = ALPHA/256
        s_offset_us = ((int64_t)TIME_SYNC_EWMA_ALPHA * raw_offset
                     + (int64_t)(256 - TIME_SYNC_EWMA_ALPHA) * s_offset_us) / 256;
    }
    s_last_rtt_us = rtt_us;
    s_sync_count++;
}

// ---------------------------------------------------------------------------
// Root (time server)
// ---------------------------------------------------------------------------

static void root_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "root socket failed"); vTaskDelete(NULL); return; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TIME_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Root time server running (µs precision)");

    char buf[16];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (1) {
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&client, &clen);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        buf[n] = '\0';
        if (strncmp(buf, "REQ", 3) != 0) continue;

        // Capture time as late as possible before sending
        uint64_t t = (uint64_t)((int64_t)esp_timer_get_time() + s_offset_us);
        uint8_t resp[8];
        encode_us(t, resp);
        sendto(sock, resp, 8, 0, (struct sockaddr *)&client, clen);
    }
}

static void start_root_task(void) {
    xTaskCreate(root_task, "time_root", 4096, NULL, 3, &s_task_handle);
}

void time_sync_start_root(void) {
    strncpy(s_role, "root", sizeof(s_role) - 1);
    start_root_task();
}

// ---------------------------------------------------------------------------
// Sync helper: burst N samples to root_ip, apply best (min-RTT) via EWMA.
// Returns true if at least one sample succeeded.
// ---------------------------------------------------------------------------

static bool do_sync_burst(int sock, const char *root_ip) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(TIME_SYNC_PORT),
    };
    inet_aton(root_ip, &addr.sin_addr);

    int64_t best_rtt    = INT64_MAX;
    int64_t best_offset = 0;
    int     good        = 0;

    for (int i = 0; i < TIME_SYNC_SAMPLES; i++) {
        int64_t t0 = (int64_t)esp_timer_get_time();
        sendto(sock, "REQ\n", 4, 0, (struct sockaddr *)&addr, sizeof(addr));

        uint8_t resp[8];
        int n = recv(sock, resp, 8, 0);

        if (n == 8) {
            int64_t t1       = (int64_t)esp_timer_get_time();
            int64_t rtt      = t1 - t0;
            int64_t root_t   = (int64_t)decode_us(resp);
            // Estimate root time at the midpoint; correct for half RTT
            int64_t offset   = root_t + rtt / 2 - (t0 + t1) / 2;

            if (rtt < best_rtt) {
                best_rtt    = rtt;
                best_offset = offset;
            }
            good++;

            if (rtt < TIME_SYNC_GOOD_RTT_US) break; // good enough, skip remaining samples
        }

        if (i < TIME_SYNC_SAMPLES - 1)
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_SAMPLE_GAP_MS));
    }

    if (good == 0) return false;

    bool was_first = s_first_sync;
    apply_offset(best_offset, best_rtt);
    ESP_LOGI(TAG, "Synced to %s: offset=%lld us  best_rtt=%lld us  (%d/%d ok)",
             root_ip, (long long)s_offset_us, (long long)best_rtt,
             good, TIME_SYNC_SAMPLES);

    if (was_first && s_first_sync_cb) {
        bool fetched = s_first_sync_cb(root_ip);
        s_first_sync_cb = NULL; // fire once only
        // If we got settings from a peer, don't apply defaults if we later win election
        if (fetched) {
            s_first_win     = false;
            s_first_win_cb  = NULL;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Follower
// ---------------------------------------------------------------------------

static char s_root_ip[16];

static void follower_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "follower socket failed"); vTaskDelete(NULL); return; }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        if (!do_sync_burst(sock, s_root_ip)) {
            s_fail_count++;
            ESP_LOGW(TAG, "Time sync to %s failed", s_root_ip);
        }
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_INTERVAL_MS));
    }
}

void time_sync_start_follower(const char *root_ip) {
    strncpy(s_root_ip, root_ip, 15);
    strncpy(s_role, "follower", sizeof(s_role) - 1);
    xTaskCreate(follower_task, "time_follower", 4096, NULL, 3, &s_task_handle);
}

// ---------------------------------------------------------------------------
// Elected mode (leader election by highest uptime, IP as tiebreaker)
// ---------------------------------------------------------------------------

static char s_my_ip[16];

static uint32_t ip_to_u32(const char *ip) {
    struct in_addr a; inet_aton(ip, &a); return ntohl(a.s_addr);
}

static void elected_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "elected socket failed"); vTaskDelete(NULL); return; }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Wait one full discovery cycle before the first election so that any
    // existing peers have had time to announce themselves.  Without this
    // delay the node wins instantly (no peers yet) and loads its local
    // preset before discovering the real root.
    vTaskDelay(pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS + 500));

    uint32_t my_u32 = ip_to_u32(s_my_ip);

    while (1) {
        peer_t peers[MAX_PEERS];
        int count = discovery_get_peers(peers, MAX_PEERS);

        // Elect the node with the highest uptime (longest-running = stable root).
        // Use IP as a tiebreaker (lower IP wins) to keep the result deterministic.
        uint32_t my_uptime = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t best_uptime = my_uptime;
        uint32_t best_u32    = my_u32;
        char best_ip[16];
        strncpy(best_ip, s_my_ip, sizeof(best_ip));

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        for (int i = 0; i < count; i++) {
            // Estimate peer's current uptime: announced value + time since we heard it
            uint32_t age_ms   = now_ms - peers[i].last_seen_ms;
            uint32_t p_uptime = peers[i].uptime_ms + age_ms;
            uint32_t p_u32    = ip_to_u32(peers[i].ip);
            if (p_uptime > best_uptime ||
                (p_uptime == best_uptime && p_u32 < best_u32)) {
                best_uptime = p_uptime;
                best_u32    = p_u32;
                strncpy(best_ip, peers[i].ip, sizeof(best_ip));
            }
        }

        if (strcmp(best_ip, s_my_ip) == 0) {
            strncpy(s_role, "root", sizeof(s_role) - 1);
            if (s_first_win && s_first_win_cb) {
                s_first_win = false;
                s_first_win_cb();
                s_first_win_cb = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_INTERVAL_MS));
            continue;
        }

        snprintf(s_role, sizeof(s_role), "->%s", best_ip);
        if (!do_sync_burst(sock, best_ip)) {
            s_fail_count++;
            ESP_LOGW(TAG, "Sync to elected root %s failed", best_ip);
        }

        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_INTERVAL_MS));
    }
}

void time_sync_start_elected(const char *my_ip) {
    strncpy(s_my_ip, my_ip, sizeof(s_my_ip) - 1);
    strncpy(s_role, "elected", sizeof(s_role) - 1);
    start_root_task();
    xTaskCreate(elected_task, "time_elected", 4096, NULL, 3, &s_task_handle);
}

uint32_t time_sync_get_stack_hwm(void) {
    return s_task_handle ? uxTaskGetStackHighWaterMark(s_task_handle) : 0;
}
