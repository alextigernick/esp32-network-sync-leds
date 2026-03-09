#include "led.h"
#include "config.h"

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

static rmt_channel_handle_t s_chan    = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

// Pixel buffer in GRB order (WS2812B wire format)
static uint8_t s_pixels[NUM_LEDS * 3];
static uint8_t s_r = 255, s_g = 255, s_b = 255;

void led_init(void) {
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &s_chan));

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = T0H,
                  .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H,
                  .level1 = 0, .duration1 = T1L },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_chan));

    // Clear strip on boot
    memset(s_pixels, 0, sizeof(s_pixels));
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(s_chan, s_encoder, s_pixels, sizeof(s_pixels), &tx_cfg));
    rmt_tx_wait_all_done(s_chan, 50);

    ESP_LOGI(TAG, "WS2812B ready: %d LEDs on GPIO %d", NUM_LEDS, LED_GPIO);
}

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    s_r = r; s_g = g; s_b = b;
}

void led_set(bool on) {
    if (on) {
        for (int i = 0; i < NUM_LEDS; i++) {
            s_pixels[i * 3 + 0] = s_g; // GRB order
            s_pixels[i * 3 + 1] = s_r;
            s_pixels[i * 3 + 2] = s_b;
        }
    } else {
        memset(s_pixels, 0, sizeof(s_pixels));
    }
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_tx_wait_all_done(s_chan, 10);
    rmt_transmit(s_chan, s_encoder, s_pixels, sizeof(s_pixels), &tx_cfg);
}
