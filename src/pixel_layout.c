#include "pixel_layout.h"
#include "config.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG  "pixel_layout"
#define PATH "/spiffs/pixel_layout.csv"

static pixel_pos_t s_pos[MAX_LEDS];
static bool        s_valid[MAX_LEDS];
static int         s_count = 0;

void pixel_layout_load(void) {
    memset(s_valid, 0, sizeof(s_valid));
    s_count = 0;

    FILE *f = fopen(PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "no pixel_layout.csv — positions unset");
        return;
    }

    char line[64];
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        char *end;
        int idx = (int)strtol(p, &end, 10);
        if (end == p || *end != ',') continue;
        p = end + 1;

        float x = strtof(p, &end);
        if (end == p || *end != ',') continue;
        p = end + 1;

        float y = strtof(p, NULL);

        if (idx >= 0 && idx < MAX_LEDS) {
            s_pos[idx].x  = x;
            s_pos[idx].y  = y;
            s_valid[idx]  = true;
            if (idx + 1 > s_count) s_count = idx + 1;
            loaded++;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "loaded %d positions (s_count=%d)", loaded, s_count);
}

bool pixel_layout_get(int idx, float *x, float *y) {
    if (idx < 0 || idx >= MAX_LEDS || !s_valid[idx]) return false;
    *x = s_pos[idx].x;
    *y = s_pos[idx].y;
    return true;
}

int pixel_layout_count(void) {
    return s_count;
}

esp_err_t pixel_layout_save_csv(const char *data, size_t len) {
    FILE *f = fopen(PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen for write failed");
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "partial write (%zu/%zu)", written, len);
        return ESP_FAIL;
    }
    pixel_layout_load();
    return ESP_OK;
}
