#include "node_config.h"
#include "config.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define TAG "node_cfg"
#define NS  "node_cfg"

static uint16_t s_num_leds = NUM_LEDS;

void node_config_load(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, using defaults (num_leds=%u)", s_num_leds);
        return;
    }
    uint16_t v;
    if (nvs_get_u16(h, "num_leds", &v) == ESP_OK && v >= 1 && v <= MAX_LEDS)
        s_num_leds = v;
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: num_leds=%u", s_num_leds);
}

uint16_t node_config_get_num_leds(void) {
    return s_num_leds;
}

void node_config_save_num_leds(uint16_t n) {
    if (n < 1)       n = 1;
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
