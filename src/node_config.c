#include "node_config.h"
#include "config.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define TAG "node_cfg"
#define NS  "node_cfg"

static strip_cfg_t s_strips[MAX_STRIPS] = {
    { .gpio = 255, .num_leds = 0 },
    { .gpio = 255, .num_leds = 0 },
    { .gpio = 255, .num_leds = 0 },
    { .gpio = 255, .num_leds = 0 },
};
static uint8_t  s_max_bright    = 255;
static int8_t   s_ct_bias       = 0;
static float    s_layout_x_mm   = 0.0f;
static float    s_layout_y_mm   = 0.0f;
static float    s_layout_rot    = 0.0f; // degrees

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

    uint32_t fraw;
    if (nvs_get_u32(h, "lay_x", &fraw) == ESP_OK)
        memcpy(&s_layout_x_mm, &fraw, 4);
    if (nvs_get_u32(h, "lay_y", &fraw) == ESP_OK)
        memcpy(&s_layout_y_mm, &fraw, 4);
    if (nvs_get_u32(h, "lay_rot", &fraw) == ESP_OK)
        memcpy(&s_layout_rot, &fraw, 4);

    nvs_close(h);

    ESP_LOGI(TAG, "loaded: strips={%u/%u, %u/%u, %u/%u, %u/%u} max_bright=%u ct_bias=%d",
             s_strips[0].gpio, s_strips[0].num_leds, s_strips[1].gpio, s_strips[1].num_leds,
             s_strips[2].gpio, s_strips[2].num_leds, s_strips[3].gpio, s_strips[3].num_leds,
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

float node_config_get_layout_x_offset(void) { return s_layout_x_mm; }
float node_config_get_layout_y_offset(void) { return s_layout_y_mm; }
float node_config_get_layout_rotation(void) { return s_layout_rot; }

void node_config_save_layout_transform(float x_mm, float y_mm, float rot_deg) {
    s_layout_x_mm = x_mm;
    s_layout_y_mm = y_mm;
    s_layout_rot  = rot_deg;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    uint32_t fraw;
    memcpy(&fraw, &x_mm,   4); nvs_set_u32(h, "lay_x",   fraw);
    memcpy(&fraw, &y_mm,   4); nvs_set_u32(h, "lay_y",   fraw);
    memcpy(&fraw, &rot_deg,4); nvs_set_u32(h, "lay_rot", fraw);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: layout_transform x=%.1f y=%.1f rot=%.1f", x_mm, y_mm, rot_deg);
}
