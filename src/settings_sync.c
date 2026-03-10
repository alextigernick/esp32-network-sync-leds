#include "settings_sync.h"
#include "discovery.h"
#include "time_sync.h"
#include "config.h"
#include "node_config.h"
#include "pixel_layout.h"
#include "perlin.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "led.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "settings"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile uint64_t s_identify_until_ms = 0;
static volatile uint32_t s_frame_count       = 0;
static volatile uint32_t s_flash_stack_hwm   = 0;
static TaskHandle_t      s_fwd_task_handle   = NULL;

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
    // Use fixed offsets — keys p0..p3 are distinct enough from pscale/pspeed/poct
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

    // Palette stop positions pp0..pp3
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
    ESP_LOGI(TAG, "applied: flash=%d period=%lums duty=%u%%",
             s->flash_enabled, (unsigned long)s->period_ms, s->duty_percent);
}

// ---------------------------------------------------------------------------
// Forward task — pushes settings to each peer via HTTP POST /settings
// ---------------------------------------------------------------------------

static void forward_task(void *arg) {
    settings_t pending;
    static char body[320];
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

// Pixel RGB buffer for pattern modes.
static uint8_t s_pattern_buf[MAX_LEDS * 3];

// Interpolate a colour from the palette for a given brightness t ∈ [0, 1].
// positions[i] are 0–255 values giving each stop's location along the gradient.
// blend: 0=linear, 1=nearest, 2=cosine, 3=step (jump at stop boundary).
// Applies pal_bright as an overall dimmer.
static void palette_lookup(float t, const uint32_t *colors, const uint8_t *positions,
                            int n, uint8_t blend, float bright_scale,
                            uint8_t *r, uint8_t *g, uint8_t *b) {
    if (n <= 0) { *r = *g = *b = 0; return; }
    if (n == 1) {
        uint32_t c = colors[0];
        *r = (uint8_t)(((c >> 16) & 0xFF) * bright_scale);
        *g = (uint8_t)(((c >>  8) & 0xFF) * bright_scale);
        *b = (uint8_t)(( c        & 0xFF) * bright_scale);
        return;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float ft = t * 255.0f;

    // Find which segment t falls in
    int idx = 0;
    for (int i = 0; i < n - 2; i++) {
        if (ft >= (float)positions[i + 1]) idx = i + 1;
        else break;
    }

    uint32_t picked;
    if (blend == 1) {
        // Nearest: pick whichever stop is closer
        float seg_start = (float)positions[idx];
        float seg_end   = (float)positions[idx + 1];
        float mid = (seg_start + seg_end) * 0.5f;
        picked = (ft < mid) ? colors[idx] : colors[idx + 1];
    } else if (blend == 3) {
        // Step: jump to next color exactly at the stop boundary
        picked = colors[idx];
    } else {
        float seg_start = (float)positions[idx];
        float seg_end   = (float)positions[idx + 1];
        float frac = (seg_end > seg_start) ? (ft - seg_start) / (seg_end - seg_start) : 0.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (blend == 2) {
            // Cosine (smoothstep S-curve)
            frac = (1.0f - cosf(frac * (float)M_PI)) * 0.5f;
        }
        // blend == 0: linear (frac unchanged)
        uint32_t c0 = colors[idx], c1 = colors[idx + 1];
        float r0 = (float)((c0 >> 16) & 0xFF), r1 = (float)((c1 >> 16) & 0xFF);
        float g0 = (float)((c0 >>  8) & 0xFF), g1 = (float)((c1 >>  8) & 0xFF);
        float b0 = (float)( c0        & 0xFF), b1 = (float)( c1        & 0xFF);
        *r = (uint8_t)((r0 + (r1 - r0) * frac) * bright_scale);
        *g = (uint8_t)((g0 + (g1 - g0) * frac) * bright_scale);
        *b = (uint8_t)((b0 + (b1 - b0) * frac) * bright_scale);
        return;
    }
    *r = (uint8_t)(((picked >> 16) & 0xFF) * bright_scale);
    *g = (uint8_t)(((picked >>  8) & 0xFF) * bright_scale);
    *b = (uint8_t)(( picked        & 0xFF) * bright_scale);
}

// Apply per-device color temperature bias to a single RGB triplet.
// ct > 0 (warm): dim blue; ct < 0 (cool): dim red.
// Scale factor 0.0025 per unit → ct=±100 gives ≤25% channel dimming.
static inline void apply_ct(uint8_t *r, uint8_t *b) {
    int8_t ct = node_config_get_ct_bias();
    if (ct > 0)
        *b = (uint8_t)(*b * (1.0f - ct * 0.0025f));
    else if (ct < 0)
        *r = (uint8_t)(*r * (1.0f + ct * 0.0025f)); // ct negative, so this dims
}

void settings_identify(uint32_t duration_ms) {
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    s_identify_until_ms = now + duration_ms;
}

uint32_t settings_get_frame_count(void)     { return s_frame_count; }
uint32_t settings_get_flash_stack_hwm(void) { return s_flash_stack_hwm; }
uint32_t settings_get_fwd_stack_hwm(void)   {
    return s_fwd_task_handle ? uxTaskGetStackHighWaterMark(s_fwd_task_handle) : 0;
}

static void flash_task(void *arg) {
    for (;;) {
        s_frame_count++;
        if ((s_frame_count & 0x3F) == 0)  // every 64 frames (~640 ms)
            s_flash_stack_hwm = uxTaskGetStackHighWaterMark(NULL);

        // Identify mode: override all LEDs with full white for duration
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (s_identify_until_ms && now_ms < s_identify_until_ms) {
            int n = node_config_get_num_leds();
            if (n > MAX_LEDS) n = MAX_LEDS;
            uint8_t id_bright = node_config_get_max_bright();
            memset(s_pattern_buf, id_bright, n * 3);
            led_write_rgb(s_pattern_buf, n);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        settings_t cur;
        settings_get(&cur);

        if (cur.mode == MODE_PERLIN || cur.mode == MODE_SINE) {
            float time_s      = (float)(time_sync_get_ms()) * 0.001f;
            float max_bright_scale = node_config_get_max_bright() / 255.0f;
            float bright_scale = (cur.pal_bright / 255.0f) * max_bright_scale;
            int   n_leds       = pixel_layout_count();
            if (n_leds > MAX_LEDS) n_leds = MAX_LEDS;

            // Pre-compute sine wave constants outside the pixel loop
            float cos_a = 0.0f, sin_a = 0.0f, time_phase = 0.0f, period_mm = 1.0f;
            float scale_mm = 1.0f, speed = 0.0f;
            int   octaves  = 1;
            if (cur.mode == MODE_SINE) {
                period_mm  = cur.sine_period_mm10 / 10.0f;
                float angle_rad = cur.sine_angle_deg10 / 10.0f * (float)(M_PI / 180.0);
                cos_a      = cosf(angle_rad);
                sin_a      = sinf(angle_rad);
                time_phase = time_s * (cur.sine_speed_c100 / 100.0f) * 2.0f * (float)M_PI;
            } else {
                scale_mm = cur.perlin_scale_mm10 / 10.0f;
                speed    = cur.perlin_speed_c100  / 400.0f;
                octaves  = (int)cur.perlin_octaves;
            }

            for (int i = 0; i < n_leds; i++) {
                float x, y;
                if (!pixel_layout_get(i, &x, &y)) {
                    s_pattern_buf[i * 3 + 0] = 0;
                    s_pattern_buf[i * 3 + 1] = 0;
                    s_pattern_buf[i * 3 + 2] = 0;
                    continue;
                }
                float t;
                if (cur.mode == MODE_SINE) {
                    float proj = x * cos_a + y * sin_a;
                    float wave = sinf((proj / period_mm) * 2.0f * (float)M_PI - time_phase);
                    t = (wave + 1.0f) * 0.5f;
                } else {
                    t = perlin_sample(x, y, time_s, scale_mm, speed, octaves) / 255.0f;
                }
                palette_lookup(t, cur.pal_colors, cur.pal_pos, (int)cur.pal_n, cur.pal_blend, bright_scale,
                               &s_pattern_buf[i*3+0],
                               &s_pattern_buf[i*3+1],
                               &s_pattern_buf[i*3+2]);
            }
            for (int i = 0; i < n_leds; i++)
                apply_ct(&s_pattern_buf[i*3+0], &s_pattern_buf[i*3+2]);
            led_write_rgb(s_pattern_buf, n_leds);
        } else {
            if (cur.flash_enabled) {
                uint64_t ms = time_sync_get_ms();
                uint32_t phase = (uint32_t)(ms % cur.period_ms);
                uint32_t on_time = (uint32_t)((uint64_t)cur.period_ms * cur.duty_percent / 100);
                bool on = (phase < on_time);
                float mb = node_config_get_max_bright() / 255.0f;
                uint8_t fr = (uint8_t)(cur.r * mb);
                uint8_t fg = (uint8_t)(cur.g * mb);
                uint8_t fb = (uint8_t)(cur.b * mb);
                apply_ct(&fr, &fb);
                led_set_rgb(fr, fg, fb);
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

void settings_start_flash_task(void) {
    s_mutex         = xSemaphoreCreateMutex();
    s_forward_queue = xQueueCreate(4, sizeof(settings_t));

    led_init();

    xTaskCreate(flash_task,   "flash",   4096, NULL, 3, NULL);
    xTaskCreate(forward_task, "fwd_set", 4096, NULL, 3, &s_fwd_task_handle);
}
