#include "settings_sync.h"
#include "discovery.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_http_client.h"
#include "esp_log.h"
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
             "&pp0=%u&pp1=%u&pp2=%u&pp3=%u&pbl=%u",
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
             (unsigned)s->pal_blend);
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
        if (v <= 2) out->mode = v;
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
// Forward task — pushes settings to each peer via HTTP POST /settings
// ---------------------------------------------------------------------------

typedef struct {
    char url[48];
    char body[320];
} fwd_peer_ctx_t;

static void forward_peer_task(void *arg) {
    fwd_peer_ctx_t *ctx = (fwd_peer_ctx_t *)arg;

    esp_http_client_config_t cfg = {
        .url        = ctx->url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = SETTINGS_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, ctx->body, (int)strlen(ctx->body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "forward to %s failed: %s", ctx->url, esp_err_to_name(err));

    esp_http_client_cleanup(client);
    free(ctx);
    vTaskDelete(NULL);
}

static void forward_task(void *arg) {
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

        peer_t peers[MAX_PEERS];
        int count = discovery_get_peers(peers, MAX_PEERS);

        for (int i = 0; i < count; i++) {
            fwd_peer_ctx_t *ctx = malloc(sizeof(fwd_peer_ctx_t));
            if (!ctx) {
                ESP_LOGW(TAG, "forward: out of memory for peer %s", peers[i].ip);
                continue;
            }
            snprintf(ctx->url, sizeof(ctx->url), "http://%s/settings?fwd=0", peers[i].ip);
            memcpy(ctx->body, body, sizeof(ctx->body));

            if (xTaskCreate(forward_peer_task, "fwd_peer", 4096, ctx, 3, NULL) != pdPASS) {
                ESP_LOGW(TAG, "forward: failed to create task for peer %s", peers[i].ip);
                free(ctx);
            }
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
}
