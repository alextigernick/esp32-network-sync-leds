#include "settings_sync.h"
#include "discovery.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "settings"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static settings_t s_settings = {
    .mode          = MODE_FLASH,
    .flash_enabled = false,
    .period_ms     = 1000,
    .duty_percent  = 50,
    .r = 10, .g = 0, .b = 0,
    .sine_period_mm10  = 1000,  // 100.0 mm
    .sine_angle_deg10  = 0,
    .sine_speed_c100   = 100,   // 1.00 Hz
    .perlin_scale_mm10 = 1000,  // 100.0 mm
    .perlin_speed_c100 = 400,   // 1.00 noise-units/s
    .perlin_octaves    = 3,
    .cx_mm10           = 0,
    .cy_mm10           = 0,
    .n_arms            = 2,
    .sparkle_density   = 128,
    .warp_strength     = 128,
    .n_seeds           = 4,
    .pal_colors        = { 0x000000, 0xFFFFFF, 0x000000, 0x000000 },
    .pal_pos           = { 0, 255, 128, 192 },
    .pal_n             = 2,     // black → white
    .pal_bright        = 255,
    .pal_blend         = 0,     // linear
};
static SemaphoreHandle_t s_mutex;
static QueueHandle_t     s_forward_queue; // queue of settings_t to forward
static TaskHandle_t      s_fwd_task_handle = NULL;

// ---------------------------------------------------------------------------
// Encode / Decode  (URL-encoded form body)
// To add a field: add one snprintf segment in encode and one strstr block in decode.
// ---------------------------------------------------------------------------

void settings_encode(const settings_t *s, char *buf, int buf_size) {
    snprintf(buf, buf_size,
             "flash=%d&period=%lu&duty=%u&r=%u&g=%u&b=%u"
             "&mode=%u&speriod=%lu&sangle=%ld&sspeed=%ld"
             "&pscale=%lu&pspeed=%ld&poct=%u"
             "&p0=%lu&p1=%lu&p2=%lu&p3=%lu&pc=%u&pbr=%u"
             "&pp0=%u&pp1=%u&pp2=%u&pp3=%u&pbl=%u"
             "&cx=%ld&cy=%ld&narms=%u&sparkd=%u&warpst=%u&nseeds=%u",
             s->flash_enabled ? 1 : 0,
             (unsigned long)s->period_ms,
             (unsigned)s->duty_percent,
             s->r, s->g, s->b,
             (unsigned)s->mode,
             (unsigned long)s->sine_period_mm10,
             (long)s->sine_angle_deg10,
             (long)s->sine_speed_c100,
             (unsigned long)s->perlin_scale_mm10,
             (long)s->perlin_speed_c100,
             (unsigned)s->perlin_octaves,
             (unsigned long)s->pal_colors[0], (unsigned long)s->pal_colors[1],
             (unsigned long)s->pal_colors[2], (unsigned long)s->pal_colors[3],
             (unsigned)s->pal_n, (unsigned)s->pal_bright,
             (unsigned)s->pal_pos[0], (unsigned)s->pal_pos[1],
             (unsigned)s->pal_pos[2], (unsigned)s->pal_pos[3],
             (unsigned)s->pal_blend,
             (long)s->cx_mm10, (long)s->cy_mm10,
             (unsigned)s->n_arms, (unsigned)s->sparkle_density,
             (unsigned)s->warp_strength, (unsigned)s->n_seeds);
}

bool settings_decode(const char *body, settings_t *out) {
    if (!body || !out) return false;

    // Start from current values so partial updates work
    settings_get(out);

    char *p;

    p = strstr(body, "flash=");
    if (p) out->flash_enabled = (p[6] == '1');

    p = strstr(body, "period=");
    if (p) {
        uint32_t v = (uint32_t)strtoul(p + 7, NULL, 10);
        if (v >= 100 && v <= 10000) out->period_ms = v;
    }

    p = strstr(body, "duty=");
    if (p) {
        uint32_t v = (uint32_t)strtoul(p + 5, NULL, 10);
        if (v >= 1 && v <= 100) out->duty_percent = (uint8_t)v;
    }

    p = strstr(body, "r=");
    if (p) out->r = (uint8_t)strtoul(p + 2, NULL, 10);
    p = strstr(body, "g=");
    if (p) out->g = (uint8_t)strtoul(p + 2, NULL, 10);
    p = strstr(body, "b=");
    if (p) out->b = (uint8_t)strtoul(p + 2, NULL, 10);

    p = strstr(body, "mode=");
    if (p) {
        uint8_t v = (uint8_t)strtoul(p + 5, NULL, 10);
        if (v <= 9) out->mode = v;
    }

    p = strstr(body, "speriod=");
    if (p) {
        uint32_t v = (uint32_t)strtoul(p + 8, NULL, 10);
        if (v >= 1) out->sine_period_mm10 = v;
    }

    p = strstr(body, "sangle=");
    if (p) out->sine_angle_deg10 = (int32_t)strtol(p + 7, NULL, 10);

    p = strstr(body, "sspeed=");
    if (p) out->sine_speed_c100 = (int32_t)strtol(p + 7, NULL, 10);

    p = strstr(body, "pscale=");
    if (p) {
        uint32_t v = (uint32_t)strtoul(p + 7, NULL, 10);
        if (v >= 1) out->perlin_scale_mm10 = v;
    }

    p = strstr(body, "pspeed=");
    if (p) out->perlin_speed_c100 = (int32_t)strtol(p + 7, NULL, 10);

    p = strstr(body, "poct=");
    if (p) {
        uint8_t v = (uint8_t)strtoul(p + 5, NULL, 10);
        if (v >= 1 && v <= 8) out->perlin_octaves = v;
    }

    // Palette
    for (int i = 0; i < 4; i++) {
        char key[4] = { 'p', (char)('0' + i), '=', '\0' };
        p = strstr(body, key);
        if (p) out->pal_colors[i] = (uint32_t)strtoul(p + 3, NULL, 10) & 0xFFFFFF;
    }

    p = strstr(body, "pc=");
    if (p) {
        uint8_t v = (uint8_t)strtoul(p + 3, NULL, 10);
        if (v >= 1 && v <= 4) out->pal_n = v;
    }

    p = strstr(body, "pbr=");
    if (p) out->pal_bright = (uint8_t)strtoul(p + 4, NULL, 10);

    for (int i = 0; i < 4; i++) {
        char key[5] = { 'p', 'p', (char)('0' + i), '=', '\0' };
        p = strstr(body, key);
        if (p) out->pal_pos[i] = (uint8_t)strtoul(p + 4, NULL, 10);
    }

    p = strstr(body, "pbl=");
    if (p) {
        uint8_t v = (uint8_t)strtoul(p + 4, NULL, 10);
        if (v <= 3) out->pal_blend = v;
    }

    p = strstr(body, "cx=");
    if (p) out->cx_mm10 = (int32_t)strtol(p + 3, NULL, 10);
    p = strstr(body, "cy=");
    if (p) out->cy_mm10 = (int32_t)strtol(p + 3, NULL, 10);

    p = strstr(body, "narms=");
    if (p) {
        uint8_t v = (uint8_t)strtoul(p + 6, NULL, 10);
        if (v >= 1 && v <= 8) out->n_arms = v;
    }

    p = strstr(body, "sparkd=");
    if (p) out->sparkle_density = (uint8_t)strtoul(p + 7, NULL, 10);

    p = strstr(body, "warpst=");
    if (p) out->warp_strength = (uint8_t)strtoul(p + 7, NULL, 10);

    p = strstr(body, "nseeds=");
    if (p) {
        uint8_t v = (uint8_t)strtoul(p + 7, NULL, 10);
        if (v >= 2 && v <= 8) out->n_seeds = v;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Get / Apply
// ---------------------------------------------------------------------------

void settings_get(settings_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_mutex);
}

void settings_apply_local(const settings_t *s) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_settings = *s;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "applied: mode=%u flash=%d period=%lums duty=%u%%",
             s->mode, s->flash_enabled, (unsigned long)s->period_ms, s->duty_percent);
}

// ---------------------------------------------------------------------------
// Subnet broadcast address — prefers STA interface, falls back to AP.
// ---------------------------------------------------------------------------

static uint32_t get_broadcast_addr(void) {
    esp_netif_ip_info_t info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0)
        return info.ip.addr | ~info.netmask.addr;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && esp_netif_get_ip_info(ap, &info) == ESP_OK && info.ip.addr != 0)
        return info.ip.addr | ~info.netmask.addr;
    return inet_addr("192.168.4.255"); // last-resort fallback
}

// ---------------------------------------------------------------------------
// Forward task — broadcasts settings via UDP, repeated SETTINGS_UDP_REPEATS
// times to cover packet loss.  Broadcast address is computed dynamically so
// this works on both the ESP32 soft-AP subnet and external routers.
// ---------------------------------------------------------------------------

static void forward_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "forward_task: socket() failed");
        vTaskDelete(NULL);
        return;
    }
    int bc = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(SETTINGS_UDP_PORT),
    };

    settings_t pending;
    char body[320];

    for (;;) {
        if (xQueueReceive(s_forward_queue, &pending, portMAX_DELAY) != pdTRUE)
            continue;

        // Drain any additional queued updates — only push the latest
        settings_t newer;
        while (xQueueReceive(s_forward_queue, &newer, 0) == pdTRUE)
            pending = newer;

        settings_encode(&pending, body, sizeof(body));
        int len = (int)strlen(body);
        dest.sin_addr.s_addr = get_broadcast_addr();

        for (int i = 0; i < SETTINGS_UDP_REPEATS; i++) {
            int sent = sendto(sock, body, len, 0,
                              (struct sockaddr *)&dest, sizeof(dest));
            if (sent < 0)
                ESP_LOGW(TAG, "broadcast send %d failed", i);
            if (i < SETTINGS_UDP_REPEATS - 1)
                vTaskDelay(pdMS_TO_TICKS(SETTINGS_UDP_REPEAT_DELAY_MS));
        }
        ESP_LOGD(TAG, "broadcast settings x%d (%d bytes)", SETTINGS_UDP_REPEATS, len);
    }
}

// ---------------------------------------------------------------------------
// UDP listener — receives settings broadcasts from peers and applies locally.
// ---------------------------------------------------------------------------

static void udp_rx_task(void *arg) {
    int sock = -1;

    for (;;) {
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock < 0) {
                ESP_LOGW(TAG, "udp_rx: socket() failed, retrying");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            int reuse = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            struct sockaddr_in bind_addr = {
                .sin_family      = AF_INET,
                .sin_port        = htons(SETTINGS_UDP_PORT),
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };
            if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
                ESP_LOGW(TAG, "udp_rx: bind() failed, retrying");
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            ESP_LOGI(TAG, "udp_rx: listening on port %d", SETTINGS_UDP_PORT);
        }

        char buf[320];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &srclen);
        if (n <= 0) {
            // timeout or transient error — keep looping
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        buf[n] = '\0';

        settings_t s;
        if (settings_decode(buf, &s)) {
            settings_apply_local(&s);
            ESP_LOGD(TAG, "udp_rx: applied settings from %s",
                     inet_ntoa(src.sin_addr));
        } else {
            ESP_LOGW(TAG, "udp_rx: decode failed (n=%d)", n);
        }
    }
}

void settings_apply_and_forward(const settings_t *s) {
    settings_apply_local(s);
    // Non-blocking enqueue — if full, drop (the latest state is already local)
    xQueueSend(s_forward_queue, s, 0);
}

// ---------------------------------------------------------------------------
// Boot-time settings fetch — pull current config from a peer on first join
// ---------------------------------------------------------------------------

bool settings_fetch_from_peer(const char *peer_ip) {
    static char url[48];
    snprintf(url, sizeof(url), "http://%s/settings", peer_ip);

    static char resp_buf[320];
    int resp_len = 0;

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = SETTINGS_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fetch from %s: open failed: %s", peer_ip, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    resp_len = esp_http_client_read(client, resp_buf, sizeof(resp_buf) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (resp_len <= 0) {
        ESP_LOGW(TAG, "fetch from %s: empty response", peer_ip);
        return false;
    }
    resp_buf[resp_len] = '\0';

    settings_t fetched;
    if (!settings_decode(resp_buf, &fetched)) {
        ESP_LOGW(TAG, "fetch from %s: decode failed", peer_ip);
        return false;
    }

    settings_apply_local(&fetched);
    ESP_LOGI(TAG, "boot-fetched settings from %s", peer_ip);
    return true;
}

// ---------------------------------------------------------------------------
// Forwarding stack diagnostic
// ---------------------------------------------------------------------------

uint32_t settings_get_fwd_stack_hwm(void) {
    return s_fwd_task_handle ? uxTaskGetStackHighWaterMark(s_fwd_task_handle) : 0;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void settings_sync_init(void) {
    s_mutex         = xSemaphoreCreateMutex();
    s_forward_queue = xQueueCreate(4, sizeof(settings_t));
    xTaskCreate(forward_task, "fwd_set", 4096, NULL, 3, &s_fwd_task_handle);
    xTaskCreate(udp_rx_task,  "set_rx",  3072, NULL, 3, NULL);
}
