#include "settings_sync.h"
#include "discovery.h"
#include "time_sync.h"
#include "config.h"
#include "node_config.h"
#include "pixel_layout.h"
#include "perlin.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
// Lookup tables (generated offline, no FPU needed at runtime)
// ---------------------------------------------------------------------------

// s_sin_lut[i] = round((sin(2π·i/256) + 1) · 127.5), index 0..255 = 0..2π
// Range [0,255].  cos(x) = s_sin_lut[(i+64)&0xFF].
static const uint8_t s_sin_lut[256] = {
    128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 162, 165, 167, 170, 173,
    176, 179, 182, 185, 188, 190, 193, 196, 198, 201, 203, 206, 208, 211, 213, 215,
    218, 220, 222, 224, 226, 228, 230, 232, 234, 235, 237, 238, 240, 241, 243, 244,
    245, 246, 248, 249, 250, 250, 251, 252, 253, 253, 254, 254, 254, 255, 255, 255,
    255, 255, 255, 255, 254, 254, 254, 253, 253, 252, 251, 250, 250, 249, 248, 246,
    245, 244, 243, 241, 240, 238, 237, 235, 234, 232, 230, 228, 226, 224, 222, 220,
    218, 215, 213, 211, 208, 206, 203, 201, 198, 196, 193, 190, 188, 185, 182, 179,
    176, 173, 170, 167, 165, 162, 158, 155, 152, 149, 146, 143, 140, 137, 134, 131,
    128, 124, 121, 118, 115, 112, 109, 106, 103, 100,  97,  93,  90,  88,  85,  82,
     79,  76,  73,  70,  67,  65,  62,  59,  57,  54,  52,  49,  47,  44,  42,  40,
     37,  35,  33,  31,  29,  27,  25,  23,  21,  20,  18,  17,  15,  14,  12,  11,
     10,   9,   7,   6,   5,   5,   4,   3,   2,   2,   1,   1,   1,   0,   0,   0,
      0,   0,   0,   0,   1,   1,   1,   2,   2,   3,   4,   5,   5,   6,   7,   9,
     10,  11,  12,  14,  15,  17,  18,  20,  21,  23,  25,  27,  29,  31,  33,  35,
     37,  40,  42,  44,  47,  49,  52,  54,  57,  59,  62,  65,  67,  70,  73,  76,
     79,  82,  85,  88,  90,  93,  97, 100, 103, 106, 109, 112, 115, 118, 121, 124,
};

// s_sin_q15[i] = round(sin(2π·i/256) · 32767), Q15 format.
// Used for dot-product projection with cos/sin of the wave angle.
static const int16_t s_sin_q15[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile uint64_t s_identify_until_ms = 0;
static volatile uint32_t s_frame_count       = 0;
static volatile uint32_t s_flash_stack_hwm   = 0;
static volatile bool     s_ota_blackout      = false;

// Profiling: rolling average of per-frame render time (µs), split by section
static volatile uint32_t s_prof_perloop_us   = 0; // per-pixel loop (noise/sine eval)
static volatile uint32_t s_prof_palette_us   = 0; // palette_lookup loop
static volatile uint32_t s_prof_ct_us        = 0; // color-temp correction loop
static volatile uint32_t s_prof_total_us     = 0; // full render (excl. led_write)
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

// Interpolate a colour from the palette.
//   t          — position in gradient, [0, 255]
//   bright_256 — overall brightness scale, [0, 255] (255 = full)
//   blend: 0=linear, 1=nearest, 2=cosine, 3=step
// All arithmetic is integer-only.
static void palette_lookup(uint8_t t, const uint32_t *colors, const uint8_t *positions,
                            int n, uint8_t blend, uint8_t bright_256,
                            uint8_t *r, uint8_t *g, uint8_t *b) {
    // Helper: scale an 8-bit channel by bright_256
    #define BRIGHT(ch) ((uint8_t)(((uint16_t)(ch) * bright_256) >> 8))

    if (n <= 0) { *r = *g = *b = 0; return; }
    if (n == 1) {
        uint32_t c = colors[0];
        *r = BRIGHT((c >> 16) & 0xFF);
        *g = BRIGHT((c >>  8) & 0xFF);
        *b = BRIGHT( c        & 0xFF);
        return;
    }

    // Find which segment t falls in
    int idx = 0;
    for (int i = 0; i < n - 2; i++) {
        if (t >= positions[i + 1]) idx = i + 1;
        else break;
    }

    uint32_t picked;
    if (blend == 1) {
        // Nearest: pick whichever stop is closer
        uint8_t mid = (uint8_t)(((uint16_t)positions[idx] + positions[idx + 1]) >> 1);
        picked = (t < mid) ? colors[idx] : colors[idx + 1];
    } else if (blend == 3) {
        // Step: current stop colour until next boundary
        picked = colors[idx];
    } else {
        uint8_t seg_start = positions[idx];
        uint8_t seg_end   = positions[idx + 1];
        // frac_256 ∈ [0, 256]: fractional position within this segment
        int32_t span = (int32_t)seg_end - seg_start;
        int32_t frac_256 = (span > 0)
            ? (int32_t)((int32_t)(t - seg_start) * 256 / span)
            : 0;
        if (frac_256 <   0) frac_256 =   0;
        if (frac_256 > 256) frac_256 = 256;

        if (blend == 2) {
            // Cosine smoothstep via sin table:
            // (1 - cos(π·frac)) / 2  =  (255 - s_sin_lut[(frac_256/2 + 64) & 0xFF]) / 255
            // Approximate as / 256 for speed (off by <0.4%).
            uint8_t cos_val = s_sin_lut[((frac_256 >> 1) + 64) & 0xFF];
            frac_256 = 255 - cos_val;
        }

        uint32_t c0 = colors[idx], c1 = colors[idx + 1];
        // Per-channel linear interpolation
        uint8_t r0 = (c0 >> 16) & 0xFF, r1 = (c1 >> 16) & 0xFF;
        uint8_t g0 = (c0 >>  8) & 0xFF, g1 = (c1 >>  8) & 0xFF;
        uint8_t b0 =  c0        & 0xFF, b1 =  c1        & 0xFF;
        uint8_t ri = (uint8_t)(r0 + ((int32_t)(r1 - r0) * frac_256 >> 8));
        uint8_t gi = (uint8_t)(g0 + ((int32_t)(g1 - g0) * frac_256 >> 8));
        uint8_t bi = (uint8_t)(b0 + ((int32_t)(b1 - b0) * frac_256 >> 8));
        *r = BRIGHT(ri);
        *g = BRIGHT(gi);
        *b = BRIGHT(bi);
        return;
    }
    *r = BRIGHT((picked >> 16) & 0xFF);
    *g = BRIGHT((picked >>  8) & 0xFF);
    *b = BRIGHT( picked        & 0xFF);
    #undef BRIGHT
}

// Apply per-device color temperature bias to a single RGB triplet.
// ct > 0 (warm): dim blue; ct < 0 (cool): dim red.
// Scale factor 0.0025 per unit → ct=±100 gives 25% channel dimming.
// Integer: scale_256 = 256 - ct * 16 / 25  (0.0025 × 256 = 0.64 = 16/25)
static inline void apply_ct(uint8_t *r, uint8_t *b) {
    int8_t ct = node_config_get_ct_bias();
    if (ct > 0) {
        int32_t scale = 256 - (int32_t)ct * 16 / 25;
        if (scale < 0) scale = 0;
        *b = (uint8_t)(((uint16_t)*b * (uint16_t)scale) >> 8);
    } else if (ct < 0) {
        int32_t scale = 256 + (int32_t)ct * 16 / 25;  // ct < 0, so this subtracts
        if (scale < 0) scale = 0;
        *r = (uint8_t)(((uint16_t)*r * (uint16_t)scale) >> 8);
    }
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
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        s_frame_count++;
        if ((s_frame_count & 0x3F) == 0)  // every 64 frames (~1280 ms at 50 Hz)
            s_flash_stack_hwm = uxTaskGetStackHighWaterMark(NULL);

        // OTA in progress: blank the strip and do nothing else this frame
        if (s_ota_blackout) {
            int n = node_config_get_num_leds();
            if (n > MAX_LEDS) n = MAX_LEDS;
            memset(s_pattern_buf, 0, n * 3);
            led_write_rgb(s_pattern_buf, n);
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
            continue;
        }

        // Identify mode: override all LEDs with full white for duration
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (s_identify_until_ms && now_ms < s_identify_until_ms) {
            int n = node_config_get_num_leds();
            if (n > MAX_LEDS) n = MAX_LEDS;
            uint8_t id_bright = node_config_get_max_bright();
            memset(s_pattern_buf, id_bright, n * 3);
            led_write_rgb(s_pattern_buf, n);
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
            continue;
        }

        settings_t cur;
        settings_get(&cur);

        if (cur.mode == MODE_PERLIN || cur.mode == MODE_SINE) {
            uint32_t time_ms   = (uint32_t)time_sync_get_ms();
            // bright_256: combined pal_bright × max_bright in [0,255]
            // (pal_bright+1)*(max_bright+1)>>8 gives exact 255 at full scale
            uint8_t max_bright = node_config_get_max_bright();
            uint8_t bright_256 = (uint8_t)(((uint16_t)cur.pal_bright * ((uint16_t)max_bright + 1)) >> 8);
            int n_leds = pixel_layout_count();
            if (n_leds > MAX_LEDS) n_leds = MAX_LEDS;

            // Pre-compute per-frame sine wave constants (angle → Q15 sin/cos)
            // angle_deg10 / 10 degrees → table index: deg * 256 / 360
            int16_t cos_q15 = 0, sin_q15_v = 0;
            int16_t period_t = 1;   // 0.1 mm units
            uint32_t sine_time_phase = 0;
            int16_t scale_t = 1;
            int     octaves = 1;
            if (cur.mode == MODE_SINE) {
                period_t = (int16_t)(cur.sine_period_mm10);  // already in 0.1mm
                // angle_deg10 → Q15 sin/cos via 256-entry table
                // idx = angle_deg10 * 256 / 3600  (256 entries = full circle)
                uint8_t angle_idx = (uint8_t)((uint32_t)cur.sine_angle_deg10 * 256 / 3600);
                sin_q15_v = s_sin_q15[angle_idx];
                cos_q15   = s_sin_q15[(angle_idx + 64) & 0xFF];
                // time_phase: 256 units = one full cycle
                // time_phase = time_ms * speed_c100 * 256 / 100000
                sine_time_phase = (uint32_t)(
                    (uint64_t)(uint32_t)time_ms * cur.sine_speed_c100 * 256 / 100000);
            } else {
                scale_t = (int16_t)(cur.perlin_scale_mm10);  // already in 0.1mm
                octaves = (int)cur.perlin_octaves;
            }

            // --- profiling timestamps ---
            int64_t t0 = esp_timer_get_time();

            // Pass 1: evaluate noise or sine per pixel → uint8_t t in [0,255]
            static uint8_t s_t_buf[MAX_LEDS];
            for (int i = 0; i < n_leds; i++) {
                int16_t x_t, y_t;
                if (!pixel_layout_get(i, &x_t, &y_t)) {
                    s_t_buf[i] = 0;
                    continue;
                }
                if (cur.mode == MODE_SINE) {
                    // Dot product projection in 0.1mm units using Q15 trig
                    int32_t proj_t = (int32_t)(
                        ((int32_t)x_t * cos_q15 + (int32_t)y_t * sin_q15_v) >> 15);
                    // Spatial phase: 256 units = one period
                    int32_t spatial_phase = (period_t > 0)
                        ? (proj_t * 256 / period_t)
                        : 0;
                    // Table lookup: subtract time_phase to animate
                    s_t_buf[i] = s_sin_lut[(uint8_t)(spatial_phase - (int32_t)sine_time_phase)];
                } else {
                    s_t_buf[i] = perlin_sample(x_t, y_t, time_ms,
                                               scale_t, cur.perlin_speed_c100, octaves);
                }
            }

            int64_t t1 = esp_timer_get_time();

            // Pass 2: palette lookup
            for (int i = 0; i < n_leds; i++) {
                palette_lookup(s_t_buf[i], cur.pal_colors, cur.pal_pos, (int)cur.pal_n,
                               cur.pal_blend, bright_256,
                               &s_pattern_buf[i*3+0],
                               &s_pattern_buf[i*3+1],
                               &s_pattern_buf[i*3+2]);
            }

            int64_t t2 = esp_timer_get_time();

            // Pass 3: color temperature correction
            for (int i = 0; i < n_leds; i++)
                apply_ct(&s_pattern_buf[i*3+0], &s_pattern_buf[i*3+2]);

            int64_t t3 = esp_timer_get_time();

            led_write_rgb(s_pattern_buf, n_leds);

            // Rolling exponential average (α = 1/8)
            uint32_t loop_us    = (uint32_t)(t1 - t0);
            uint32_t palette_us = (uint32_t)(t2 - t1);
            uint32_t ct_us      = (uint32_t)(t3 - t2);
            uint32_t total_us   = (uint32_t)(t3 - t0);
            s_prof_perloop_us = (s_prof_perloop_us * 7 + loop_us)    / 8;
            s_prof_palette_us = (s_prof_palette_us * 7 + palette_us) / 8;
            s_prof_ct_us      = (s_prof_ct_us      * 7 + ct_us)      / 8;
            s_prof_total_us   = (s_prof_total_us   * 7 + total_us)   / 8;

            // Log every 64 frames (~640 ms)
            if ((s_frame_count & 0x3F) == 0) {
                ESP_LOGI(TAG, "render [%s] noise/sine=%"PRIu32"µs palette=%"PRIu32"µs ct=%"PRIu32"µs total=%"PRIu32"µs leds=%d",
                         cur.mode == MODE_SINE ? "sine" : "perlin",
                         s_prof_perloop_us, s_prof_palette_us, s_prof_ct_us, s_prof_total_us, n_leds);
            }
        } else {
            if (cur.flash_enabled) {
                uint64_t ms = time_sync_get_ms();
                uint32_t phase = (uint32_t)(ms % cur.period_ms);
                uint32_t on_time = (uint32_t)((uint64_t)cur.period_ms * cur.duty_percent / 100);
                bool on = (phase < on_time);
                uint8_t mb = node_config_get_max_bright();
                // Scale each channel: (ch * (mb+1)) >> 8  gives correct 0..255 range
                uint8_t fr = (uint8_t)(((uint16_t)cur.r * ((uint16_t)mb + 1)) >> 8);
                uint8_t fg = (uint8_t)(((uint16_t)cur.g * ((uint16_t)mb + 1)) >> 8);
                uint8_t fb = (uint8_t)(((uint16_t)cur.b * ((uint16_t)mb + 1)) >> 8);
                apply_ct(&fr, &fb);
                led_set_rgb(fr, fg, fb);
                led_set(on);
            } else {
                led_set(false);
            }
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20)); // 50 Hz
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

void settings_ota_blackout(bool enable) {
    s_ota_blackout = enable;
}

void settings_start_flash_task(void) {
    s_mutex         = xSemaphoreCreateMutex();
    s_forward_queue = xQueueCreate(4, sizeof(settings_t));

    led_init();

    xTaskCreate(flash_task,   "flash",   4096, NULL, 3, NULL);
    xTaskCreate(forward_task, "fwd_set", 4096, NULL, 3, &s_fwd_task_handle);
}
