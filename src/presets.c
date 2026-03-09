#include "presets.h"
#include "settings_sync.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>
#include <stdio.h>

#define TAG "presets"
#define NS  "presets"

/* NVS layout (namespace "presets"):
 *   cnt   uint8   number of saved presets (0-PRESETS_MAX)
 *   n0-n7 blob    preset name strings (null-terminated, ≤ PRESET_NAME_MAX)
 *   v0-v7 blob    URL-encoded settings strings
 *   def   blob    boot-default preset name (empty string = no default)
 */

/* ---- helpers ---- */

static void idx_key(char *key_out, char prefix, int idx) {
    /* produces "n0".."n7" or "v0".."v7" */
    key_out[0] = prefix;
    key_out[1] = '0' + idx;
    key_out[2] = '\0';
}

static int find_slot(nvs_handle_t h, const char *name) {
    uint8_t cnt = 0;
    nvs_get_u8(h, "cnt", &cnt);
    char key[4], buf[PRESET_NAME_MAX];
    for (int i = 0; i < (int)cnt; i++) {
        idx_key(key, 'n', i);
        size_t sz = sizeof(buf);
        if (nvs_get_blob(h, key, buf, &sz) == ESP_OK && strcmp(buf, name) == 0)
            return i;
    }
    return -1;
}

/* ---- public API ---- */

int presets_list(preset_info_t *infos, int max) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t cnt = 0;
    nvs_get_u8(h, "cnt", &cnt);
    if ((int)cnt > max) cnt = (uint8_t)max;

    char key[4];
    for (int i = 0; i < (int)cnt; i++) {
        idx_key(key, 'n', i);
        size_t sz = PRESET_NAME_MAX;
        infos[i].name[0] = '\0';
        nvs_get_blob(h, key, infos[i].name, &sz);
        infos[i].name[PRESET_NAME_MAX - 1] = '\0';
    }
    nvs_close(h);
    return cnt;
}

bool presets_save(const char *name, const settings_t *s) {
    if (!name || !name[0]) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;

    uint8_t cnt = 0;
    nvs_get_u8(h, "cnt", &cnt);

    int slot = find_slot(h, name);
    if (slot < 0) {
        if (cnt >= PRESETS_MAX) {
            ESP_LOGW(TAG, "preset store full");
            nvs_close(h);
            return false;
        }
        slot = cnt;
        cnt++;
    }

    char key[4];
    /* store name */
    idx_key(key, 'n', slot);
    nvs_set_blob(h, key, name, strlen(name) + 1);

    /* encode and store settings */
    static char enc[512];
    settings_encode(s, enc, sizeof(enc));
    idx_key(key, 'v', slot);
    nvs_set_blob(h, key, enc, strlen(enc) + 1);

    nvs_set_u8(h, "cnt", cnt);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "saved preset '%s' at slot %d", name, slot);
    return true;
}

bool presets_load(const char *name, settings_t *s_out) {
    if (!name || !name[0]) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    int slot = find_slot(h, name);
    if (slot < 0) {
        nvs_close(h);
        return false;
    }

    char key[4];
    static char enc[512];
    size_t sz = sizeof(enc);
    idx_key(key, 'v', slot);
    esp_err_t err = nvs_get_blob(h, key, enc, &sz);
    nvs_close(h);

    if (err != ESP_OK) return false;
    enc[sizeof(enc) - 1] = '\0';

    settings_t tmp;
    settings_get(&tmp);                  /* start from current so missing fields keep values */
    if (!settings_decode(enc, &tmp)) return false;
    *s_out = tmp;
    return true;
}

bool presets_delete(const char *name) {
    if (!name || !name[0]) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;

    uint8_t cnt = 0;
    nvs_get_u8(h, "cnt", &cnt);

    int slot = find_slot(h, name);
    if (slot < 0) {
        nvs_close(h);
        return false;
    }

    /* shift remaining slots down */
    char key_src[4], key_dst[4];
    char buf[512];
    for (int i = slot; i < (int)cnt - 1; i++) {
        idx_key(key_src, 'n', i + 1);
        idx_key(key_dst, 'n', i);
        size_t sz = sizeof(buf);
        if (nvs_get_blob(h, key_src, buf, &sz) == ESP_OK)
            nvs_set_blob(h, key_dst, buf, sz);

        idx_key(key_src, 'v', i + 1);
        idx_key(key_dst, 'v', i);
        sz = sizeof(buf);
        if (nvs_get_blob(h, key_src, buf, &sz) == ESP_OK)
            nvs_set_blob(h, key_dst, buf, sz);
    }

    /* erase the last slot (now a duplicate) */
    cnt--;
    idx_key(key_dst, 'n', cnt);
    nvs_erase_key(h, key_dst);
    idx_key(key_dst, 'v', cnt);
    nvs_erase_key(h, key_dst);
    nvs_set_u8(h, "cnt", cnt);

    /* clear default if it matched */
    char def[PRESET_NAME_MAX] = {0};
    size_t dsz = sizeof(def);
    nvs_get_blob(h, "def", def, &dsz);
    if (strcmp(def, name) == 0) nvs_erase_key(h, "def");

    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "deleted preset '%s'", name);
    return true;
}

bool presets_set_default(const char *name) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (!name || !name[0]) {
        nvs_erase_key(h, "def");
    } else {
        nvs_set_blob(h, "def", name, strlen(name) + 1);
    }
    nvs_commit(h);
    nvs_close(h);
    return true;
}

bool presets_get_default(char *out, int out_size) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) { out[0] = '\0'; return false; }
    size_t sz = (size_t)out_size;
    esp_err_t err = nvs_get_blob(h, "def", out, &sz);
    nvs_close(h);
    if (err != ESP_OK) { out[0] = '\0'; return false; }
    out[out_size - 1] = '\0';
    return out[0] != '\0';
}

void presets_apply_default(void) {
    char def[PRESET_NAME_MAX] = {0};
    if (!presets_get_default(def, sizeof(def))) return;

    settings_t s;
    if (!presets_load(def, &s)) {
        ESP_LOGW(TAG, "default preset '%s' not found", def);
        return;
    }
    settings_apply_local(&s);
    ESP_LOGI(TAG, "applied default preset '%s'", def);
}

int presets_to_json(char *buf, int buf_size) {
    preset_info_t infos[PRESETS_MAX];
    int cnt = presets_list(infos, PRESETS_MAX);

    char def[PRESET_NAME_MAX] = {0};
    presets_get_default(def, sizeof(def));

    int pos = 0;
#define A(...) pos += snprintf(buf + pos, buf_size - pos, __VA_ARGS__)
    A("{\"default\":\"%s\",\"presets\":[", def);
    for (int i = 0; i < cnt; i++) {
        /* simple JSON string escaping — names are user text, escape \ and " */
        A("%s{\"name\":\"", i > 0 ? "," : "");
        for (const char *p = infos[i].name; *p && pos < buf_size - 4; p++) {
            if (*p == '"' || *p == '\\') buf[pos++] = '\\';
            buf[pos++] = *p;
        }
        A("\",\"is_default\":%s}", strcmp(infos[i].name, def) == 0 ? "true" : "false");
    }
    A("]}");
#undef A
    return pos;
}
