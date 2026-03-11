#include "led.h"
#include "config.h"
#include "node_config.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "soc/rmt_periph.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "led"

// WS2812B timing at 10 MHz (1 tick = 100 ns)
#define RMT_RESOLUTION_HZ  10000000
#define T0H  4   // 400 ns
#define T0L  8   // 800 ns
#define T1H  8   // 800 ns
#define T1L  4   // 400 ns

#define NUM_RMT_CHANNELS 2

static rmt_channel_handle_t s_chan[NUM_RMT_CHANNELS] = {0};
static rmt_encoder_handle_t s_encoder[NUM_RMT_CHANNELS] = {0};
static int s_chan_gpio[NUM_RMT_CHANNELS];  // current GPIO assigned to each channel
static SemaphoreHandle_t s_mutex = NULL;

static uint8_t  s_strip_gpio[MAX_STRIPS];
static uint16_t s_strip_leds[MAX_STRIPS];
static uint16_t s_strip_offset[MAX_STRIPS];
static uint8_t  s_strip_channel[MAX_STRIPS];

static int      s_num_strips = 0;
static uint16_t s_total_leds = 0;

static uint8_t  s_pixels[MAX_LEDS * 3];

static uint8_t s_r = 10, s_g = 0, s_b = 0;
static bool s_probe_mode = false;


// ---------------------------------------------------------------------------
// Strip configuration
// ---------------------------------------------------------------------------

static void init_strips_from_config(void)
{
    strip_cfg_t strips[MAX_STRIPS];
    int count;

    node_config_get_strips(strips, &count);

    s_num_strips = 0;
    s_total_leds = 0;

    for (int i = 0; i < MAX_STRIPS; i++) {
        s_strip_gpio[i] = 255;
        s_strip_leds[i] = 0;
        s_strip_offset[i] = 0;
        s_strip_channel[i] = 0;
    }

    for (int i = 0; i < count && i < MAX_STRIPS; i++) {

        if (strips[i].gpio == 255 || strips[i].num_leds == 0)
            continue;

        int si = s_num_strips;

        s_strip_gpio[si]   = strips[i].gpio;
        s_strip_leds[si]   = strips[i].num_leds;
        s_strip_offset[si] = s_total_leds;

        // distribute strips across the two channels
        s_strip_channel[si] = si % NUM_RMT_CHANNELS;

        s_total_leds += strips[i].num_leds;
        s_num_strips++;

        ESP_LOGI(TAG,
                 "strip %d: GPIO %u LEDs %u offset %u ch %u",
                 si,
                 s_strip_gpio[si],
                 s_strip_leds[si],
                 s_strip_offset[si],
                 s_strip_channel[si]);
    }

    if (s_total_leds == 0)
        s_total_leds = 1;
}


// ---------------------------------------------------------------------------
// RMT setup
// ---------------------------------------------------------------------------

static void create_rmt_channels(void)
{
    for (int i = 0; i < NUM_RMT_CHANNELS; i++)
        s_chan_gpio[i] = -1;

    if (s_num_strips == 0)
        return;

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = T0H,
                  .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H,
                  .level1 = 0, .duration1 = T1L },
        .flags.msb_first = 1,
    };

    for (int ch = 0; ch < NUM_RMT_CHANNELS; ch++) {
        ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_encoder[ch]));
    }

    for (int ch = 0; ch < NUM_RMT_CHANNELS; ch++) {

        int gpio = -1;

        for (int i = 0; i < s_num_strips; i++) {
            if (s_strip_channel[i] == ch) {
                gpio = s_strip_gpio[i];
                break;
            }
        }

        if (gpio < 0 || !GPIO_IS_VALID_GPIO(gpio)) {
            s_chan[ch] = NULL;
            continue;
        }

        rmt_tx_channel_config_t chan_cfg = {
            .gpio_num = gpio,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = RMT_RESOLUTION_HZ,
            .mem_block_symbols = 48,
            .trans_queue_depth = 4,
            .intr_priority = 3,
        };

        ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &s_chan[ch]));
        s_chan_gpio[ch] = gpio;
        gpio_set_level(gpio, 0);
        gpio_pullup_dis(gpio);
        gpio_pulldown_dis(gpio);
        ESP_LOGI(TAG, "Registered tx channel\n");

    }
    for (int ch = 0; ch < NUM_RMT_CHANNELS; ch++) {
        if (s_chan[ch])
            ESP_ERROR_CHECK(rmt_enable(s_chan[ch]));
    }
    
}


static void delete_rmt_channels(void)
{
    for (int i = 0; i < NUM_RMT_CHANNELS; i++) {

        if (!s_chan[i])
            continue;

        rmt_tx_wait_all_done(s_chan[i], 50);
        rmt_disable(s_chan[i]);
        rmt_del_channel(s_chan[i]);
        s_chan[i] = NULL;
    }

    for (int i = 0; i < NUM_RMT_CHANNELS; i++) {
        if (s_encoder[i]) {
            rmt_del_encoder(s_encoder[i]);
            s_encoder[i] = NULL;
        }
    }
}


// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void led_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    init_strips_from_config();
    create_rmt_channels();

    memset(s_pixels, 0, sizeof(s_pixels));

    ESP_LOGI(TAG,
             "WS2812 ready: %d strips, %u LEDs",
             s_num_strips,
             s_total_leds);
}


void led_reinit(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    delete_rmt_channels();
    init_strips_from_config();
    create_rmt_channels();

    memset(s_pixels, 0, sizeof(s_pixels));

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG,
             "WS2812 reinit: %d strips, %u LEDs",
             s_num_strips,
             s_total_leds);
}


static void transmit_all(void)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0
    };

    // transmit remaining strips in parallel pairs
    for (int base = 0; base < s_num_strips; base += NUM_RMT_CHANNELS) {


        for (int ch = 0; ch < NUM_RMT_CHANNELS; ch++) {
            int si = base + ch;
            if (si >= s_num_strips)
                continue;
            if (s_chan_gpio[ch] != s_strip_gpio[si]) {
                rmt_disable(s_chan[ch]);
                rmt_tx_switch_gpio(s_chan[ch], s_strip_gpio[si], false);
                PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[s_strip_gpio[si]], PIN_FUNC_GPIO);
                rmt_enable(s_chan[ch]);
                s_chan_gpio[ch] = s_strip_gpio[si];
            }

        }

        for (int ch = 0; ch < NUM_RMT_CHANNELS; ch++) {

            int si = base + ch;
            if (si >= s_num_strips)
                continue;


            rmt_transmit(
                s_chan[ch],
                s_encoder[ch],
                s_pixels + s_strip_offset[si] * 3,
                s_strip_leds[si] * 3,
                &tx_cfg);
                
        }

        // wait for both channels
        for (int ch = 0; ch < NUM_RMT_CHANNELS; ch++) {
            if (s_chan[ch])
                rmt_tx_wait_all_done(s_chan[ch], 100);
        }
        // ESP_LOGI(TAG,
        //      "done");
        esp_rom_delay_us(60);
    }
    xSemaphoreGive(s_mutex);
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r;
    s_g = g;
    s_b = b;
}


void led_set_pixel(int idx)
{
    s_probe_mode = (idx >= 0);

    memset(s_pixels, 0, s_total_leds * 3);

    if (idx >= 0 && idx < s_total_leds) {
        // Always use full white for probe pixels — calibration always sends
        // r=255 g=255 b=255 via allLedsOff, but s_r/s_g/s_b are only updated
        // by flash_task when flash_enabled=true, so rely on them being 255 here
        // is unreliable. Full white is always correct for LED detection.
        uint8_t bright = node_config_get_max_bright();
        s_pixels[idx * 3 + 0] = bright;  // g channel (WS2812B GRB order)
        s_pixels[idx * 3 + 1] = bright;  // r channel
        s_pixels[idx * 3 + 2] = bright;  // b channel
    }

    transmit_all();
}


void led_write_rgb(const uint8_t *rgb, int count)
{
    s_probe_mode = false;

    if (count > s_total_leds)
        count = s_total_leds;

    for (int i = 0; i < count; i++) {
        s_pixels[i * 3 + 0] = rgb[i * 3 + 1];
        s_pixels[i * 3 + 1] = rgb[i * 3 + 0];
        s_pixels[i * 3 + 2] = rgb[i * 3 + 2];
    }

    if (count < s_total_leds)
        memset(s_pixels + count * 3, 0, (s_total_leds - count) * 3);

    transmit_all();
}


void led_set(bool on)
{
    if (s_probe_mode)
        return;

    if (on) {

        for (int i = 0; i < s_total_leds; i++) {

            s_pixels[i * 3 + 0] = s_g;
            s_pixels[i * 3 + 1] = s_r;
            s_pixels[i * 3 + 2] = s_b;
        }

    } else {

        memset(s_pixels, 0, s_total_leds * 3);
    }

    transmit_all();
}