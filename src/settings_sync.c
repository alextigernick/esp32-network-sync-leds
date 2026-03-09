#include "settings_sync.h"
#include "discovery.h"
#include "time_sync.h"
#include "config.h"
#include "pixel_layout.h"
#include "perlin.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "led.h"
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
    .mode          = MODE_FLASH,
    .flash_enabled = false,
    .period_ms     = 1000,
    .duty_percent  = 50,
    .r = 10, .g = 0, .b = 0,
    .sine_period_mm10  = 1000,  // 100.0 mm
    .sine_angle_deg10  = 0,
    .sine_speed_c100   = 100,   // 1.00 Hz
    .perlin_scale_mm10 = 1000,  // 100.0 mm
    .perlin_speed_c100 = 100,   // 1.00 noise-units/s
};
static SemaphoreHandle_t s_mutex;
static QueueHandle_t     s_forward_queue; // queue of settings_t to forward

// ---------------------------------------------------------------------------
// Encode / Decode  (URL-encoded form body)
// To add a field: add one snprintf segment in encode and one strstr block in decode.
// ---------------------------------------------------------------------------

void settings_encode(const settings_t *s, char *buf, int buf_size) {
    snprintf(buf, buf_size,
             "flash=%d&period=%lu&duty=%u&r=%u&g=%u&b=%u"
             "&mode=%u&speriod=%lu&sangle=%ld&sspeed=%ld"
             "&pscale=%lu&pspeed=%ld",
             s->flash_enabled ? 1 : 0,
             (unsigned long)s->period_ms,
             (unsigned)s->duty_percent,
             s->r, s->g, s->b,
             (unsigned)s->mode,
             (unsigned long)s->sine_period_mm10,
             (long)s->sine_angle_deg10,
             (long)s->sine_speed_c100,
             (unsigned long)s->perlin_scale_mm10,
             (long)s->perlin_speed_c100);
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
    static char body[192];
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

// Pixel RGB buffer for sine mode (MAX_LEDS * 3 bytes).
static uint8_t s_sine_buf[MAX_LEDS * 3];

static void flash_task(void *arg) {
    for (;;) {
        settings_t cur;
        settings_get(&cur);

        if (cur.mode == MODE_PERLIN) {
            float scale_mm = cur.perlin_scale_mm10 / 10.0f;
            float speed    = cur.perlin_speed_c100  / 100.0f;
            float time_s   = (float)(time_sync_get_ms()) * 0.001f;
            int n_leds     = pixel_layout_count();
            if (n_leds > MAX_LEDS) n_leds = MAX_LEDS;

            for (int i = 0; i < n_leds; i++) {
                float x, y;
                if (!pixel_layout_get(i, &x, &y)) {
                    s_sine_buf[i * 3 + 0] = 0;
                    s_sine_buf[i * 3 + 1] = 0;
                    s_sine_buf[i * 3 + 2] = 0;
                    continue;
                }
                float bright = perlin_sample(x, y, time_s, scale_mm, speed) / 255.0f;
                s_sine_buf[i * 3 + 0] = (uint8_t)(bright * cur.r);
                s_sine_buf[i * 3 + 1] = (uint8_t)(bright * cur.g);
                s_sine_buf[i * 3 + 2] = (uint8_t)(bright * cur.b);
            }
            led_write_rgb(s_sine_buf, n_leds);
        } else if (cur.mode == MODE_SINE) {
            // Compute wave parameters from fixed-point settings
            float period_mm  = cur.sine_period_mm10 / 10.0f;
            float angle_rad  = cur.sine_angle_deg10 / 10.0f * (float)(M_PI / 180.0);
            float speed_hz   = cur.sine_speed_c100  / 100.0f;
            float cos_a      = cosf(angle_rad);
            float sin_a      = sinf(angle_rad);
            float time_s     = (float)(time_sync_get_ms()) * 0.001f;
            float time_phase = time_s * speed_hz * 2.0f * (float)M_PI;
            int n_leds       = pixel_layout_count();
            if (n_leds > MAX_LEDS) n_leds = MAX_LEDS;

            for (int i = 0; i < n_leds; i++) {
                float x, y;
                if (!pixel_layout_get(i, &x, &y)) {
                    s_sine_buf[i * 3 + 0] = 0;
                    s_sine_buf[i * 3 + 1] = 0;
                    s_sine_buf[i * 3 + 2] = 0;
                    continue;
                }
                // Project pixel position onto wave direction
                float proj    = x * cos_a + y * sin_a;
                float wave    = sinf((proj / period_mm) * 2.0f * (float)M_PI - time_phase);
                float bright  = (wave + 1.0f) * 0.5f; // 0..1
                s_sine_buf[i * 3 + 0] = (uint8_t)(bright * cur.r);
                s_sine_buf[i * 3 + 1] = (uint8_t)(bright * cur.g);
                s_sine_buf[i * 3 + 2] = (uint8_t)(bright * cur.b);
            }
            led_write_rgb(s_sine_buf, n_leds);
        } else {
            if (cur.flash_enabled) {
                uint64_t ms = time_sync_get_ms();
                uint32_t phase = (uint32_t)(ms % cur.period_ms);
                uint32_t on_time = (uint32_t)((uint64_t)cur.period_ms * cur.duty_percent / 100);
                bool on = (phase < on_time);
                led_set_rgb(cur.r, cur.g, cur.b);
                led_set(on);
            } else {
                led_set(false);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // 10 ms resolution
    }
}

// ---------------------------------------------------------------------------
// Boot-time settings fetch — pull current config from a peer on first join
// ---------------------------------------------------------------------------

bool settings_fetch_from_peer(const char *peer_ip) {
    static char url[48];
    snprintf(url, sizeof(url), "http://%s/settings", peer_ip);

    static char resp_buf[256];
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

void settings_start_flash_task(void) {
    s_mutex         = xSemaphoreCreateMutex();
    s_forward_queue = xQueueCreate(4, sizeof(settings_t));

    led_init();

    xTaskCreate(flash_task,   "flash",   4096, NULL, 3, NULL);
    xTaskCreate(forward_task, "fwd_set", 4096, NULL, 3, NULL);
}
