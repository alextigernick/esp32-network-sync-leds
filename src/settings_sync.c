#include "settings_sync.h"
#include "discovery.h"
#include "time_sync.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "driver/gpio.h"
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

static settings_t      s_settings = {
    .flash_enabled = false,
    .period_ms     = 1000,
    .duty_percent  = 50,
};
static SemaphoreHandle_t s_mutex;
static QueueHandle_t     s_forward_queue; // queue of settings_t to forward

// ---------------------------------------------------------------------------
// Encode / Decode  (URL-encoded form body)
// To add a field: add one snprintf segment in encode and one strstr block in decode.
// ---------------------------------------------------------------------------

void settings_encode(const settings_t *s, char *buf, int buf_size) {
    snprintf(buf, buf_size,
             "flash=%d&period=%lu&duty=%u",
             s->flash_enabled ? 1 : 0,
             (unsigned long)s->period_ms,
             (unsigned)s->duty_percent);
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
    ESP_LOGI(TAG, "applied: flash=%d period=%lums duty=%u%%",
             s->flash_enabled, (unsigned long)s->period_ms, s->duty_percent);
}

// ---------------------------------------------------------------------------
// Forward task — pushes settings to each peer via HTTP POST /settings
// ---------------------------------------------------------------------------

static void forward_task(void *arg) {
    settings_t pending;
    static char body[64];
    static char url[48];

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
            snprintf(url, sizeof(url), "http://%s/settings?fwd=0", peers[i].ip);

            esp_http_client_config_t cfg = {
                .url             = url,
                .method          = HTTP_METHOD_POST,
                .timeout_ms      = SETTINGS_HTTP_TIMEOUT_MS,
                .skip_cert_common_name_check = true,
            };
            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            esp_http_client_set_header(client, "Content-Type",
                                       "application/x-www-form-urlencoded");
            esp_http_client_set_post_field(client, body, (int)strlen(body));

            esp_err_t err = esp_http_client_perform(client);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "forward to %s failed: %s",
                         peers[i].ip, esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }
    }
}

void settings_apply_and_forward(const settings_t *s) {
    settings_apply_local(s);
    // Non-blocking enqueue — if full, drop (the latest state is already local)
    xQueueSend(s_forward_queue, s, 0);
}

// ---------------------------------------------------------------------------
// Flash task — drives GPIO based on synced time
// ---------------------------------------------------------------------------

static void flash_task(void *arg) {
    for (;;) {
        settings_t cur;
        settings_get(&cur);

        if (cur.flash_enabled) {
            uint64_t ms = time_sync_get_ms();
            uint32_t phase = (uint32_t)(ms % cur.period_ms);
            uint32_t on_time = (uint32_t)((uint64_t)cur.period_ms * cur.duty_percent / 100);
            gpio_set_level(LED_GPIO, phase < on_time ? 1 : 0);
        } else {
            gpio_set_level(LED_GPIO, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // 10 ms resolution
    }
}

void settings_start_flash_task(void) {
    s_mutex         = xSemaphoreCreateMutex();
    s_forward_queue = xQueueCreate(4, sizeof(settings_t));

    xTaskCreate(flash_task,   "flash",   2048, NULL, 3, NULL);
    xTaskCreate(forward_task, "fwd_set", 4096, NULL, 3, NULL);
}
