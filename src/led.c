#include "led.h"
#include "config.h"
#include "node_config.h"

#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define TAG "led"

// WS2812B timing at 10 MHz (1 tick = 100 ns)
#define RMT_RESOLUTION_HZ  10000000
#define T0H  4   // 400 ns
#define T0L  8   // 800 ns
#define T1H  8   // 800 ns
#define T1L  4   // 400 ns

static rmt_channel_handle_t s_chan[MAX_STRIPS]    = {NULL};
static rmt_encoder_handle_t s_encoder[MAX_STRIPS] = {NULL};
static uint16_t             s_strip_leds[MAX_STRIPS] = {0};
static int                  s_num_strips = 0;
static uint16_t             s_total_leds = 0;

// Pixel buffer in GRB order (WS2812B wire format) for all strips combined.
static uint8_t  s_pixels[MAX_LEDS * 3];
static uint8_t  s_r = 10, s_g = 0, s_b = 0;
static bool     s_probe_mode = false;

static void init_strips_from_config(void) {
    strip_cfg_t strips[MAX_STRIPS];
    int count;
    node_config_get_strips(strips, &count);

    s_num_strips = 0;
    s_total_leds = 0;

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = T0H,
                  .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H,
                  .level1 = 0, .duration1 = T1L },
        .flags.msb_first = 1,
    };

    for (int i = 0; i < count && i < MAX_STRIPS; i++) {
        if (strips[i].gpio == 255 || strips[i].num_leds == 0)
            continue;

        rmt_tx_channel_config_t chan_cfg = {
            .gpio_num          = strips[i].gpio,
            .clk_src           = RMT_CLK_SRC_DEFAULT,
            .resolution_hz     = RMT_RESOLUTION_HZ,
            .mem_block_symbols = 64,
            .trans_queue_depth = 4,
        };
        int si = s_num_strips;
        ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &s_chan[si]));
        ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_encoder[si]));
        ESP_ERROR_CHECK(rmt_enable(s_chan[si]));

        s_strip_leds[si] = strips[i].num_leds;
        s_total_leds    += strips[i].num_leds;
        s_num_strips++;

        ESP_LOGI(TAG, "strip %d: GPIO %u, %u LEDs", si, strips[i].gpio, strips[i].num_leds);
    }
}

static void delete_strips(void) {
    for (int i = 0; i < s_num_strips; i++) {
        if (s_chan[i]) {
            rmt_tx_wait_all_done(s_chan[i], 50);
            rmt_disable(s_chan[i]);
            rmt_del_channel(s_chan[i]);
            s_chan[i] = NULL;
        }
        if (s_encoder[i]) {
            rmt_del_encoder(s_encoder[i]);
            s_encoder[i] = NULL;
        }
        s_strip_leds[i] = 0;
    }
    s_num_strips = 0;
    s_total_leds = 0;
}

// Transmit the flat s_pixels buffer, splitting across strips by their LED counts.
static void transmit_all(void) {
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    uint16_t offset = 0;
    for (int i = 0; i < s_num_strips; i++) {
        rmt_tx_wait_all_done(s_chan[i], 10);
        rmt_transmit(s_chan[i], s_encoder[i],
                     s_pixels + offset * 3, s_strip_leds[i] * 3, &tx_cfg);
        offset += s_strip_leds[i];
    }
}

void led_init(void) {
    init_strips_from_config();

    // Clear all strips on boot
    memset(s_pixels, 0, s_total_leds * 3);
    transmit_all();

    ESP_LOGI(TAG, "WS2812B ready: %d strips, %u total LEDs", s_num_strips, s_total_leds);
}

void led_reinit(void) {
    delete_strips();
    init_strips_from_config();
    memset(s_pixels, 0, s_total_leds * 3);
    transmit_all();
    ESP_LOGI(TAG, "WS2812B reinit: %d strips, %u total LEDs", s_num_strips, s_total_leds);
}

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    s_r = r; s_g = g; s_b = b;
}

void led_set_pixel(int idx) {
    s_probe_mode = (idx >= 0);
    memset(s_pixels, 0, s_total_leds * 3);
    if (idx >= 0 && idx < s_total_leds) {
        s_pixels[idx * 3 + 0] = s_g; // GRB order
        s_pixels[idx * 3 + 1] = s_r;
        s_pixels[idx * 3 + 2] = s_b;
    }
    transmit_all();
}

void led_write_rgb(const uint8_t *rgb, int count) {
    s_probe_mode = false;
    if (count > s_total_leds) count = s_total_leds;
    for (int i = 0; i < count; i++) {
        s_pixels[i * 3 + 0] = rgb[i * 3 + 1]; // G
        s_pixels[i * 3 + 1] = rgb[i * 3 + 0]; // R
        s_pixels[i * 3 + 2] = rgb[i * 3 + 2]; // B
    }
    if (count < s_total_leds)
        memset(s_pixels + count * 3, 0, (s_total_leds - count) * 3);
    transmit_all();
}

void led_set(bool on) {
    if (s_probe_mode) return;
    if (on) {
        for (int i = 0; i < s_total_leds; i++) {
            s_pixels[i * 3 + 0] = s_g; // GRB order
            s_pixels[i * 3 + 1] = s_r;
            s_pixels[i * 3 + 2] = s_b;
        }
    } else {
        memset(s_pixels, 0, s_total_leds * 3);
    }
    transmit_all();
}
