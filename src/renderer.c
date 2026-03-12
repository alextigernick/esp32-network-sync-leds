#include "renderer.h"
#include "settings_sync.h"
#include "time_sync.h"
#include "node_config.h"
#include "pixel_layout.h"
#include "perlin.h"
#include "led.h"
#include "config.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "renderer"

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
static volatile uint32_t s_stack_hwm         = 0;
static volatile bool     s_ota_blackout      = false;

// Profiling: rolling average of per-frame render time (µs), split by section
static volatile uint32_t s_prof_perloop_us = 0;
static volatile uint32_t s_prof_palette_us = 0;
static volatile uint32_t s_prof_ct_us      = 0;
static volatile uint32_t s_prof_total_us   = 0;

// Per-pixel RGB output buffer
static uint8_t s_pattern_buf[MAX_LEDS * 3];

// ---------------------------------------------------------------------------
// Math helpers — integer-only, no FPU
// ---------------------------------------------------------------------------

// Integer square root (Babylonian), 5–6 iterations, exact for n < 2^32.
static uint32_t isqrt32(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n, y = (n + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

// Fast atan2 returning [0, 256) = [0°, 360°) CCW from +x axis.
// Maps each 45° octant linearly to 32 units.  Max error < 1°.
static uint8_t atan2_u8(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;
    int32_t ax = x < 0 ? -x : x;
    int32_t ay = y < 0 ? -y : y;
    // oct ∈ [0, 32]: linear approximation within the first octant (0–45°).
    // Multiplier 32 so that 256 units = full circle (64 units = 90°).
    uint8_t oct = (ax >= ay)
        ? (uint8_t)(ay * 32 / (ax ? ax : 1))
        : (uint8_t)(64 - ax * 32 / (ay ? ay : 1));
    if (x >= 0 && y >= 0) return oct;
    if (x <  0 && y >= 0) return (uint8_t)(128 - oct);
    if (x <  0)           return (uint8_t)(128 + oct);
    return (uint8_t)(0   - oct);  // x >= 0, y < 0 → wraps to 256-oct
}

// Pixel hash for sparkle — stable per-pixel phase offset.
static inline uint8_t pixel_hash(int i) {
    uint32_t h = (uint32_t)(i + 1) * 2654435761u;
    h ^= h >> 16;
    return (uint8_t)(h >> 8);
}

// ---------------------------------------------------------------------------
// Palette lookup
// ---------------------------------------------------------------------------

// Interpolate a colour from the palette.
//   t          — position in gradient, [0, 255]
//   bright_256 — overall brightness scale, [0, 255] (255 = full)
//   blend: 0=linear, 1=nearest, 2=cosine, 3=step
// All arithmetic is integer-only.
static void palette_lookup(uint8_t t, const uint32_t *colors, const uint8_t *positions,
                            int n, uint8_t blend, uint8_t bright_256,
                            uint8_t *r, uint8_t *g, uint8_t *b) {
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

// ---------------------------------------------------------------------------
// Color temperature correction
// ---------------------------------------------------------------------------

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
        int32_t scale = 256 + (int32_t)ct * 16 / 25;
        if (scale < 0) scale = 0;
        *r = (uint8_t)(((uint16_t)*r * (uint16_t)scale) >> 8);
    }
}

// ---------------------------------------------------------------------------
// Render task
// ---------------------------------------------------------------------------

static void render_task(void *arg) {
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        s_frame_count++;
        if ((s_frame_count & 0x3F) == 0)  // every 64 frames (~1280 ms at 50 Hz)
            s_stack_hwm = uxTaskGetStackHighWaterMark(NULL);

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

        if (cur.mode != MODE_FLASH) {
            uint32_t time_ms = (uint32_t)time_sync_get_ms();
            uint8_t max_bright = node_config_get_max_bright();
            uint8_t bright_256 = (uint8_t)(((uint16_t)cur.pal_bright * ((uint16_t)max_bright + 1)) >> 8);
            int n_leds = pixel_layout_count();
            if (n_leds > MAX_LEDS) n_leds = MAX_LEDS;

            // --- Per-frame constants shared across modes ---
            int32_t period_t  = (int32_t)cur.sine_period_mm10;   // 0.1 mm
            int32_t scale_t   = (int32_t)cur.perlin_scale_mm10;  // 0.1 mm
            int     octaves   = (int)cur.perlin_octaves;
            int32_t cx_t      = cur.cx_mm10;
            int32_t cy_t      = cur.cy_mm10;

            uint8_t angle_idx   = (uint8_t)((uint32_t)cur.sine_angle_deg10 * 256 / 3600);
            int16_t sin_q15_v   = s_sin_q15[angle_idx];
            int16_t cos_q15     = s_sin_q15[(angle_idx + 64) & 0xFF];
            // time_phase: 256 units = one full period (for sine-speed modes)
            uint32_t time_phase  = (uint32_t)((uint64_t)time_ms * cur.sine_speed_c100   * 256 / 100000);
            // ptime_phase: same but driven by perlin_speed (for perlin/plasma/voronoi/ripple alts)
            uint32_t ptime_phase = (uint32_t)((uint64_t)time_ms * cur.perlin_speed_c100 * 256 / 100000);

            // Pre-compute Voronoi seed positions (only used in MODE_VORONOI)
            static int32_t s_vseed_x[8], s_vseed_y[8];
            if (cur.mode == MODE_VORONOI) {
                uint8_t n = cur.n_seeds;
                if (n < 2) n = 2;
                uint8_t base = (uint8_t)ptime_phase;
                for (int si = 0; si < n; si++) {
                    uint8_t ang = (uint8_t)(base + (uint32_t)si * 256 / n);
                    s_vseed_x[si] = cx_t + ((int32_t)s_sin_q15[(ang + 64) & 0xFF] * scale_t >> 15);
                    s_vseed_y[si] = cy_t + ((int32_t)s_sin_q15[ang]               * scale_t >> 15);
                }
            }

            int64_t t0 = esp_timer_get_time();

            // Pass 1: evaluate pattern per pixel → s_t_buf[i] in [0,255]
            static uint8_t s_t_buf[MAX_LEDS];
            for (int i = 0; i < n_leds; i++) {
                int16_t x_t, y_t;
                if (!pixel_layout_get(i, &x_t, &y_t)) { s_t_buf[i] = 0; continue; }

                switch (cur.mode) {

                case MODE_SINE: {
                    // Traveling plane wave — same as before
                    int32_t proj = ((int32_t)x_t * cos_q15 + (int32_t)y_t * sin_q15_v) >> 15;
                    int32_t sp   = (period_t > 0) ? (proj * 256 / period_t) : 0;
                    s_t_buf[i]   = s_sin_lut[(uint8_t)(sp - (int32_t)time_phase)];
                    break;
                }

                case MODE_PERLIN:
                    s_t_buf[i] = perlin_sample(x_t, y_t, time_ms,
                                               (int16_t)scale_t, cur.perlin_speed_c100, octaves);
                    break;

                case MODE_PLASMA: {
                    // 4 overlapping sine waves → interference (demoscene plasma)
                    // Each wave has a different spatial direction; all share the time phase.
                    uint8_t p1 = (uint8_t)((int32_t)x_t       * 256 / (scale_t ? scale_t : 1) - (int32_t)ptime_phase);
                    uint8_t p2 = (uint8_t)((int32_t)y_t       * 256 / (scale_t ? scale_t : 1) - (int32_t)ptime_phase * 3 / 4);
                    int32_t scale2 = scale_t * 2; if (scale2 == 0) scale2 = 1;
                    uint8_t p3 = (uint8_t)(((int32_t)x_t + y_t) * 256 / scale2 - (int32_t)ptime_phase / 2);
                    uint32_t r  = isqrt32((uint32_t)((int32_t)x_t * x_t) + (uint32_t)((int32_t)y_t * y_t));
                    uint8_t p4  = (uint8_t)(r * 256 / (scale_t ? scale_t : 1) + (int32_t)ptime_phase / 3);
                    int32_t v   = (int32_t)s_sin_lut[p1] + s_sin_lut[p2] + s_sin_lut[p3] + s_sin_lut[p4];
                    s_t_buf[i]  = (uint8_t)(v / 4);
                    break;
                }

                case MODE_RIPPLE: {
                    // Concentric rings radiating from (cx, cy)
                    int32_t dx  = (int32_t)x_t - cx_t;
                    int32_t dy  = (int32_t)y_t - cy_t;
                    uint32_t r  = isqrt32((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
                    int32_t sp  = (period_t > 0) ? ((int32_t)r * 256 / period_t) : 0;
                    s_t_buf[i]  = s_sin_lut[(uint8_t)(sp - (int32_t)time_phase)];
                    break;
                }

                case MODE_SPIRAL: {
                    // Archimedean spiral: angle × n_arms + radial phase
                    int32_t dx  = (int32_t)x_t - cx_t;
                    int32_t dy  = (int32_t)y_t - cy_t;
                    uint8_t ang = atan2_u8(dy, dx);                       // [0,256) = 2π
                    uint32_t r  = isqrt32((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
                    uint8_t rp  = (uint8_t)((period_t > 0) ? (r * 256 / (uint32_t)period_t) : 0);
                    uint8_t t_s = (uint8_t)((uint16_t)ang * cur.n_arms + rp - (uint8_t)time_phase);
                    s_t_buf[i]  = s_sin_lut[t_s];
                    break;
                }

                case MODE_SPARKLE: {
                    // Per-pixel independent twinkle.
                    //
                    // Each pixel runs its own phase-shifted sine wave.  The
                    // active/inactive decision re-rolls once per period, but only
                    // at the pixel's OWN zero crossing — exactly when brightness
                    // is already 0 — so there is never a visible jump.
                    //
                    // Waveform: 0 → peak → 0 over one period (LUT offset by 192
                    // so the trough aligns with the epoch boundary).
                    uint32_t period   = cur.period_ms > 0 ? cur.period_ms : 1000;

                    // Fixed per-pixel phase offset — spreads zero crossings across
                    // the period so the active set refreshes gradually, not all at once.
                    uint32_t ph32 = (uint32_t)(i + 1) * 2246822519u;
                    ph32 ^= ph32 >> 16;
                    uint8_t phase_off = (uint8_t)(ph32 >> 8);

                    // Pixel's own clock: shift by phase offset so each pixel's
                    // epoch boundary (zero crossing) falls at a different time.
                    uint32_t t_px     = time_ms + (uint32_t)phase_off * period / 256;
                    uint32_t px_epoch = t_px / period;
                    uint32_t px_pos   = t_px % period;

                    // Active/inactive re-rolls only at the epoch boundary (bri = 0).
                    uint32_t h32 = ((uint32_t)(i + 1) ^
                                    (px_epoch * 1664525u + 1013904223u)) * 2654435761u;
                    h32 ^= h32 >> 16;

                    if ((uint8_t)(h32 >> 8) < cur.sparkle_density) {
                        // +192 shifts so lut[0+192]=0 (trough), giving 0→peak→0 envelope.
                        uint8_t lut_idx = (uint8_t)(px_pos * 256 / period) + 192;
                        s_t_buf[i] = s_sin_lut[lut_idx];
                    } else {
                        s_t_buf[i] = 0;
                    }
                    break;
                }

                case MODE_WARP: {
                    // Domain-warped Perlin: displace sampling coords by two Perlin values.
                    int32_t wx  = (int32_t)perlin_sample(x_t, y_t, time_ms,
                                      (int16_t)scale_t, cur.perlin_speed_c100, octaves) - 128;
                    int32_t wy  = (int32_t)perlin_sample((int16_t)(x_t + scale_t),
                                      (int16_t)(y_t + scale_t), time_ms,
                                      (int16_t)scale_t, cur.perlin_speed_c100, octaves) - 128;
                    int32_t ws  = (int32_t)cur.warp_strength * scale_t / 256;
                    int16_t wx2 = (int16_t)(x_t + wx * ws / 128);
                    int16_t wy2 = (int16_t)(y_t + wy * ws / 128);
                    s_t_buf[i]  = perlin_sample(wx2, wy2, time_ms,
                                      (int16_t)scale_t, cur.perlin_speed_c100, octaves);
                    break;
                }

                case MODE_STANDING: {
                    // 2-D standing wave on rotated axes: sin(u/λ) × sin(v/λ)
                    int32_t u   = ((int32_t)x_t *  cos_q15 + (int32_t)y_t * sin_q15_v) >> 15;
                    int32_t v   = ((int32_t)x_t * -sin_q15_v + (int32_t)y_t * cos_q15) >> 15;
                    int32_t su  = (int32_t)s_sin_lut[(uint8_t)(u * 256 / (period_t ? period_t : 1) - (int32_t)time_phase)] - 128;
                    int32_t sv  = (int32_t)s_sin_lut[(uint8_t)(v * 256 / (period_t ? period_t : 1))] - 128;
                    int32_t val = su * sv / 128 + 128;
                    s_t_buf[i]  = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val);
                    break;
                }

                case MODE_VORONOI: {
                    // Distance to nearest orbiting seed → cell shading
                    uint8_t n       = cur.n_seeds;
                    if (n < 2) n    = 2;
                    uint32_t min_d2 = UINT32_MAX;
                    for (int si = 0; si < n; si++) {
                        int32_t ddx = (int32_t)x_t - s_vseed_x[si];
                        int32_t ddy = (int32_t)y_t - s_vseed_y[si];
                        uint32_t d2 = (uint32_t)(ddx * ddx) + (uint32_t)(ddy * ddy);
                        if (d2 < min_d2) min_d2 = d2;
                    }
                    uint32_t dist   = isqrt32(min_d2);
                    int32_t  vi     = 255 - (int32_t)dist * 255 / (scale_t ? scale_t : 1);
                    s_t_buf[i]      = (uint8_t)(vi < 0 ? 0 : (vi > 255 ? 255 : vi));
                    break;
                }

                default:
                    s_t_buf[i] = 0;
                    break;
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

            uint32_t loop_us    = (uint32_t)(t1 - t0);
            uint32_t palette_us = (uint32_t)(t2 - t1);
            uint32_t ct_us      = (uint32_t)(t3 - t2);
            uint32_t total_us   = (uint32_t)(t3 - t0);
            s_prof_perloop_us = (s_prof_perloop_us * 7 + loop_us)    / 8;
            s_prof_palette_us = (s_prof_palette_us * 7 + palette_us) / 8;
            s_prof_ct_us      = (s_prof_ct_us      * 7 + ct_us)      / 8;
            s_prof_total_us   = (s_prof_total_us   * 7 + total_us)   / 8;

            if ((s_frame_count & 0x3F) == 0) {
                static const char *const mode_names[] = {
                    "flash","sine","perlin","plasma","ripple",
                    "spiral","sparkle","warp","standing","voronoi"
                };
                const char *mn = (cur.mode < 10) ? mode_names[cur.mode] : "?";
                ESP_LOGI(TAG, "render [%s] pixel=%"PRIu32"µs palette=%"PRIu32"µs ct=%"PRIu32"µs total=%"PRIu32"µs leds=%d",
                         mn, s_prof_perloop_us, s_prof_palette_us, s_prof_ct_us, s_prof_total_us, n_leds);
            }
        } else {
            // MODE_FLASH
            if (cur.flash_enabled) {
                uint64_t ms = time_sync_get_ms();
                uint32_t phase = (uint32_t)(ms % cur.period_ms);
                uint32_t on_time = (uint32_t)((uint64_t)cur.period_ms * cur.duty_percent / 100);
                bool on = (phase < on_time);
                uint8_t mb = node_config_get_max_bright();
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
// Public API
// ---------------------------------------------------------------------------

void renderer_start(void) {
    led_init();
    xTaskCreate(render_task, "render", 4096, NULL, 3, NULL);
}

void renderer_set_ota_blackout(bool enable) {
    s_ota_blackout = enable;
}

void renderer_identify(uint32_t duration_ms) {
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    s_identify_until_ms = now + duration_ms;
}

uint32_t renderer_get_frame_count(void) { return s_frame_count; }
uint32_t renderer_get_stack_hwm(void)   { return s_stack_hwm; }
