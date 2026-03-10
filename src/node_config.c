#include "node_config.h"
#include "config.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define TAG "node_cfg"
#define NS  "node_cfg"

static uint16_t s_num_leds   = NUM_LEDS;
static uint8_t  s_max_bright = 255;
static int8_t   s_ct_bias    = 0;

void node_config_load(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, using defaults (num_leds=%u)", s_num_leds);
        return;
    }
    uint16_t v;
    if (nvs_get_u16(h, "num_leds", &v) == ESP_OK && v >= 1 && v <= MAX_LEDS)
        s_num_leds = v;
    uint8_t b;
    if (nvs_get_u8(h, "max_bright", &b) == ESP_OK)
        s_max_bright = b;
    int8_t ct;
    if (nvs_get_i8(h, "ct_bias", &ct) == ESP_OK)
        s_ct_bias = ct;
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: num_leds=%u max_bright=%u ct_bias=%d",
             s_num_leds, s_max_bright, (int)s_ct_bias);
}

uint16_t node_config_get_num_leds(void) {
    return s_num_leds;
}

void node_config_save_num_leds(uint16_t n) {
    if (n < 1)        n = 1;
    if (n > MAX_LEDS) n = MAX_LEDS;
    s_num_leds = n;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_u16(h, "num_leds", n);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: num_leds=%u", n);
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
