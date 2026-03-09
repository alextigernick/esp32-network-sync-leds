#include "grid_config.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define TAG "grid_cfg"
#define NS  "grid_cfg"

static grid_config_t s_cfg = {
    .rows          = 4,
    .cols          = 8,
    .origin        = 0,  // TL
    .row_first     = 1,
    .x_spacing_mm  = 50.0f,
    .y_spacing_mm  = 50.0f,
};

void grid_config_load(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, using defaults (%ux%u)", s_cfg.rows, s_cfg.cols);
        return;
    }
    grid_config_t tmp;
    size_t sz = sizeof(tmp);
    if (nvs_get_blob(h, "cfg", &tmp, &sz) == ESP_OK && sz == sizeof(tmp)
            && tmp.rows >= 1 && tmp.rows <= 32
            && tmp.cols >= 1 && tmp.cols <= 32
            && tmp.origin <= 3) {
        s_cfg = tmp;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: %ux%u origin=%u row_first=%u",
             s_cfg.rows, s_cfg.cols, s_cfg.origin, s_cfg.row_first);
}

void grid_config_get(grid_config_t *out) {
    *out = s_cfg;
}

void grid_config_save(const grid_config_t *cfg) {
    s_cfg = *cfg;
    if (s_cfg.rows < 1)  s_cfg.rows = 1;
    if (s_cfg.rows > 32) s_cfg.rows = 32;
    if (s_cfg.cols < 1)  s_cfg.cols = 1;
    if (s_cfg.cols > 32) s_cfg.cols = 32;
    if (s_cfg.origin > 3) s_cfg.origin = 0;
    s_cfg.row_first = s_cfg.row_first ? 1 : 0;
    if (s_cfg.x_spacing_mm <= 0.0f) s_cfg.x_spacing_mm = 1.0f;
    if (s_cfg.y_spacing_mm <= 0.0f) s_cfg.y_spacing_mm = 1.0f;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_blob(h, "cfg", &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: %ux%u origin=%u row_first=%u spacing=%.2fx%.2fmm",
             s_cfg.rows, s_cfg.cols, s_cfg.origin, s_cfg.row_first,
             s_cfg.x_spacing_mm, s_cfg.y_spacing_mm);
}
