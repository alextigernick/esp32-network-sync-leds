#include "node_config.h"
#include "config.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define TAG "node_cfg"
#define NS  "node_cfg"

static strip_cfg_t s_strips[MAX_STRIPS] = {
    { .gpio = LED_GPIO, .num_leds = NUM_LEDS },
    { .gpio = 255,      .num_leds = 0        },
};
static uint8_t s_max_bright = 255;
static int8_t  s_ct_bias    = 0;

void node_config_load(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        return;
    }

    for (int i = 0; i < MAX_STRIPS; i++) {
        char key_gpio[16], key_leds[16];
        snprintf(key_gpio, sizeof(key_gpio), "s%d_gpio", i);
        snprintf(key_leds, sizeof(key_leds), "s%d_leds", i);
        uint8_t g;
        if (nvs_get_u8(h, key_gpio, &g) == ESP_OK)
            s_strips[i].gpio = g;
        uint16_t n;
        if (nvs_get_u16(h, key_leds, &n) == ESP_OK && n <= MAX_LEDS)
            s_strips[i].num_leds = n;
    }

    uint8_t b;
    if (nvs_get_u8(h, "max_bright", &b) == ESP_OK)
        s_max_bright = b;
    int8_t ct;
    if (nvs_get_i8(h, "ct_bias", &ct) == ESP_OK)
        s_ct_bias = ct;

    nvs_close(h);

    ESP_LOGI(TAG, "loaded: strip0={gpio=%u,leds=%u} strip1={gpio=%u,leds=%u} max_bright=%u ct_bias=%d",
             s_strips[0].gpio, s_strips[0].num_leds,
             s_strips[1].gpio, s_strips[1].num_leds,
             s_max_bright, (int)s_ct_bias);
}

void node_config_get_strips(strip_cfg_t *out, int *count_out) {
    for (int i = 0; i < MAX_STRIPS; i++)
        out[i] = s_strips[i];
    *count_out = MAX_STRIPS;
}

void node_config_save_strip(int i, uint8_t gpio, uint16_t num_leds) {
    if (i < 0 || i >= MAX_STRIPS) return;
    if (num_leds > MAX_LEDS) num_leds = MAX_LEDS;
    s_strips[i].gpio     = gpio;
    s_strips[i].num_leds = num_leds;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    char key_gpio[16], key_leds[16];
    snprintf(key_gpio, sizeof(key_gpio), "s%d_gpio", i);
    snprintf(key_leds, sizeof(key_leds), "s%d_leds", i);
    nvs_set_u8(h, key_gpio, gpio);
    nvs_set_u16(h, key_leds, num_leds);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved strip %d: gpio=%u leds=%u", i, gpio, num_leds);
}

uint16_t node_config_get_num_leds(void) {
    uint16_t total = 0;
    for (int i = 0; i < MAX_STRIPS; i++) {
        if (s_strips[i].gpio != 255)
            total += s_strips[i].num_leds;
    }
    return total > 0 ? total : 1;
}

uint8_t node_config_get_max_bright(void) {
    return s_max_bright;
}

int8_t node_config_get_ct_bias(void) {
    return s_ct_bias;
}

void node_config_save_ct_bias(int8_t v) {
    if (v < -100) v = -100;
    if (v >  100) v =  100;
    s_ct_bias = v;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_i8(h, "ct_bias", v);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: ct_bias=%d", (int)v);
}

void node_config_save_max_bright(uint8_t v) {
    s_max_bright = v;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_u8(h, "max_bright", v);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: max_bright=%u", v);
}
