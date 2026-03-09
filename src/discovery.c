#include "discovery.h"
#include "config.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "discovery"

// Announce packet format: "NAME:<name>\nIP:<ip>\nUPTIME:<ms>\n"
#define ANNOUNCE_FMT "NAME:%s\nIP:%s\nUPTIME:%lu\n"

static peer_t s_peers[MAX_PEERS];
static int    s_peer_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static char s_my_ip[16];
static char s_my_name[PEER_NAME_LEN];

// ---- peer table helpers ------------------------------------------------

static void peer_update(const char *ip, const char *name, uint32_t uptime_ms) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i].ip, ip) == 0) {
            strncpy(s_peers[i].name, name, PEER_NAME_LEN - 1);
            s_peers[i].last_seen_ms = now;
            s_peers[i].uptime_ms    = uptime_ms;
            xSemaphoreGive(s_mutex);
            return;
        }
    }
    if (s_peer_count < MAX_PEERS) {
        strncpy(s_peers[s_peer_count].ip,   ip,   15);
        strncpy(s_peers[s_peer_count].name, name, PEER_NAME_LEN - 1);
        s_peers[s_peer_count].last_seen_ms = now;
        s_peers[s_peer_count].uptime_ms    = uptime_ms;
        s_peer_count++;
        ESP_LOGI(TAG, "New peer: %s (%s)", name, ip);
    }
    xSemaphoreGive(s_mutex);
}

static void peer_expire(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_peer_count; ) {
        if (now - s_peers[i].last_seen_ms > DISCOVERY_INTERVAL_MS * 4) {
            ESP_LOGI(TAG, "Peer expired: %s", s_peers[i].ip);
            s_peers[i] = s_peers[--s_peer_count];
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_mutex);
}

// ---- tasks -------------------------------------------------------------

static void announce_task(void *arg) {
    ESP_LOGI(TAG, "announce_task started");
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "announce socket failed"); vTaskDelete(NULL); return; }
    ESP_LOGI(TAG, "announce socket ok");

    int ttl = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(DISCOVERY_PORT),
    };
    inet_aton(DISCOVERY_MCAST_ADDR, &dest.sin_addr);

    char buf[128];
    while (1) {
        uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        int len = snprintf(buf, sizeof(buf), ANNOUNCE_FMT, s_my_name, s_my_ip, uptime_ms);
        sendto(sock, buf, len, 0, (struct sockaddr *)&dest, sizeof(dest));
        peer_expire();
        vTaskDelay(pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS));
    }
}

static void listen_task(void *arg) {
    ESP_LOGI(TAG, "listen_task started");
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "listen socket failed"); vTaskDelete(NULL); return; }
    ESP_LOGI(TAG, "listen socket ok");

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind failed"); close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "listen bind ok");

    struct ip_mreq mreq;
    inet_aton(DISCOVERY_MCAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    int join = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    ESP_LOGI(TAG, "multicast join: %s", join == 0 ? "ok" : "FAILED");

    char buf[256];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    // Set a receive timeout so the task yields even when no packets arrive
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));  // yield on timeout/error
            continue;
        }
        buf[n] = '\0';

        // Parse NAME, IP, and UPTIME fields
        char name[PEER_NAME_LEN] = {0};
        char ip[16] = {0};
        uint32_t uptime_ms = 0;
        char *p;
        if ((p = strstr(buf, "NAME:")) != NULL) {
            sscanf(p + 5, "%31[^\n]", name);
        }
        if ((p = strstr(buf, "IP:")) != NULL) {
            sscanf(p + 3, "%15[^\n]", ip);
        }
        if ((p = strstr(buf, "UPTIME:")) != NULL) {
            sscanf(p + 7, "%lu", &uptime_ms);
        }

        // Ignore our own announcements
        if (strcmp(ip, s_my_ip) == 0) continue;

        if (ip[0] && name[0]) {
            peer_update(ip, name, uptime_ms);
        }
    }
}

// ---- public API --------------------------------------------------------

void discovery_start(const char *my_ip, const char *my_name) {
    s_mutex = xSemaphoreCreateMutex();
    strncpy(s_my_ip,   my_ip,   15);
    strncpy(s_my_name, my_name, PEER_NAME_LEN - 1);

    xTaskCreate(listen_task,   "disc_listen",   6144, NULL, 3, NULL);
    xTaskCreate(announce_task, "disc_announce", 4096, NULL, 3, NULL);
}

int discovery_get_peers(peer_t *out, int max) {
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_peer_count < max ? s_peer_count : max;
    memcpy(out, s_peers, n * sizeof(peer_t));
    xSemaphoreGive(s_mutex);
    return n;
}
