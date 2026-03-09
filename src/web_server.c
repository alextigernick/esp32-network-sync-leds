/*
 * REST API
 * --------
 *
 * GET /
 *   Returns the embedded web UI (text/html).
 *
 * GET /state
 *   Returns node state as JSON:
 *     { "led": bool,
 *       "sync_ms": uint64,
 *       "ota_pending": bool,
 *       "ts_role": "master"|"slave",
 *       "ts_offset_us": int64,
 *       "ts_rtt_us": int64,
 *       "ts_sync_count": uint32,
 *       "ts_fail_count": uint32,
 *       "flash_enabled": bool,
 *       "period_ms": uint32,
 *       "duty_percent": uint8,
 *       "r": uint8, "g": uint8, "b": uint8,
 *       "peers": [{"name": str, "ip": str}, ...] }
 *
 * POST /led
 *   Body (form-encoded): state=on | state=off
 *   Turns GPIO 20 on or off immediately.
 *   Response: 204 No Content
 *
 * POST /ota
 *   Body: multipart/form-data with one file part containing a valid ESP-IDF
 *   OTA firmware image (.bin). Node reboots into the new firmware on success.
 *   Response: 200 "OK" (then reboots)
 *
 * POST /ota/verify
 *   Marks the running firmware as valid, cancelling any pending rollback.
 *   Response: 200 plain-text status message
 *
 * GET /settings
 *   Returns current synchronized settings as URL-encoded form:
 *     flash=0|1&period=<ms>&duty=<1-100>&r=<0-255>&g=<0-255>&b=<0-255>
 *
 * POST /settings[?fwd=0]
 *   Body (form-encoded): same fields as GET response (all optional, current
 *   values kept for missing fields).
 *     flash=0|1            — enable/disable synchronized flashing
 *     period=<100-10000>   — flash period in ms
 *     duty=<1-100>         — on-time percentage per period
 *     r=<0-255>            — LED red channel
 *     g=<0-255>            — LED green channel
 *     b=<0-255>            — LED blue channel
 *   Applies locally and forwards to all peers unless ?fwd=0 is set.
 *   Response: 204 No Content
 *
 * GET /node_config
 *   Returns per-node config as JSON:
 *     { "num_leds": uint16 }
 *
 * POST /node_config
 *   Body (form-encoded): num_leds=<1-500>
 *   Persists and applies LED strip length immediately.
 *   Response: 204 No Content
 *
 * POST /led_pixel
 *   Body (form-encoded): idx=<0-499> | idx=-1
 *   Lights a single LED at the given index for visual identification;
 *   idx=-1 turns off the test pixel.
 *   Response: 204 No Content
 *
 * GET /pixel_layout
 *   Returns the node's pixel position map as raw CSV (text/plain):
 *     # comment lines ignored
 *     <index>,<x_mm>,<y_mm>
 *   One line per LED. Indices need not be contiguous or ordered.
 *   Returns an empty body if no layout has been uploaded yet.
 *
 * POST /pixel_layout
 *   Body (text/plain): CSV in the same format as GET.
 *   Overwrites /spiffs/pixel_layout.csv and reloads the in-memory
 *   position table. No size limit beyond the SPIFFS partition (448 KB).
 *   Response: 204 No Content
 */

#include "web_server.h"
#include "discovery.h"
#include "node_config.h"
#include "pixel_layout.h"
#include "presets.h"
#include "time_sync.h"
#include "settings_sync.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "led.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "web_server"

extern const uint8_t web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const uint8_t web_ui_html_end[]   asm("_binary_web_ui_html_end");

static bool s_led_on = false;

static void web_led_set(bool on) {
    s_led_on = on;
    led_set(on);
}

// ---- /state JSON endpoint ----------------------------------------------

static esp_err_t handle_state(httpd_req_t *req) {
    peer_t peers[MAX_PEERS];
    int peer_count = discovery_get_peers(peers, MAX_PEERS);
    uint64_t sync_ms = time_sync_get_ms();

    // Check OTA pending-verify state
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_VALID;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_get_state_partition(running, &ota_state);
    bool ota_pending = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);

    time_sync_debug_t dbg;
    time_sync_get_debug(&dbg);

    static char buf[1280];
    int pos = 0;

#define A(...) pos += snprintf(buf + pos, sizeof(buf) - pos, __VA_ARGS__)

    settings_t cfg;
    settings_get(&cfg);

    A("{\"led\":%s,\"sync_ms\":%llu,\"ota_pending\":%s"
      ",\"ts_role\":\"%s\",\"ts_offset_us\":%lld,\"ts_rtt_us\":%lld"
      ",\"ts_sync_count\":%lu,\"ts_fail_count\":%lu"
      ",\"flash_enabled\":%s,\"period_ms\":%lu,\"duty_percent\":%u"
      ",\"r\":%u,\"g\":%u,\"b\":%u"
      ",\"mode\":%u,\"sine_period_mm10\":%lu,\"sine_angle_deg10\":%ld,\"sine_speed_c100\":%ld"
      ",\"perlin_scale_mm10\":%lu,\"perlin_speed_c100\":%ld,\"perlin_octaves\":%u"
      ",\"pal_n\":%u,\"pal_bright\":%u"
      ",\"pal0\":%lu,\"pal1\":%lu,\"pal2\":%lu,\"pal3\":%lu"
      ",\"palp0\":%u,\"palp1\":%u,\"palp2\":%u,\"palp3\":%u"
      ",\"pal_blend\":%u"
      ",\"peers\":[",
      s_led_on ? "true" : "false",
      (unsigned long long)sync_ms,
      ota_pending ? "true" : "false",
      dbg.role,
      (long long)dbg.offset_us,
      (long long)dbg.last_rtt_us,
      (unsigned long)dbg.sync_count,
      (unsigned long)dbg.fail_count,
      cfg.flash_enabled ? "true" : "false",
      (unsigned long)cfg.period_ms,
      (unsigned)cfg.duty_percent,
      (unsigned)cfg.r, (unsigned)cfg.g, (unsigned)cfg.b,
      (unsigned)cfg.mode,
      (unsigned long)cfg.sine_period_mm10,
      (long)cfg.sine_angle_deg10,
      (long)cfg.sine_speed_c100,
      (unsigned long)cfg.perlin_scale_mm10,
      (long)cfg.perlin_speed_c100,
      (unsigned)cfg.perlin_octaves,
      (unsigned)cfg.pal_n, (unsigned)cfg.pal_bright,
      (unsigned long)cfg.pal_colors[0], (unsigned long)cfg.pal_colors[1],
      (unsigned long)cfg.pal_colors[2], (unsigned long)cfg.pal_colors[3],
      (unsigned)cfg.pal_pos[0], (unsigned)cfg.pal_pos[1],
      (unsigned)cfg.pal_pos[2], (unsigned)cfg.pal_pos[3],
      (unsigned)cfg.pal_blend);

    for (int i = 0; i < peer_count; i++) {
        A("%s{\"name\":\"%s\",\"ip\":\"%s\"}",
          i > 0 ? "," : "", peers[i].name, peers[i].ip);
    }
    A("]}");

#undef A

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

// ---- / HTML page -------------------------------------------------------

static esp_err_t handle_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)web_ui_html_start,
                    web_ui_html_end - web_ui_html_start);
    return ESP_OK;
}

// ---- /led POST ---------------------------------------------------------

static esp_err_t handle_led_post(httpd_req_t *req) {
    char body[64] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    if      (strstr(body, "state=on"))  web_led_set(true);
    else if (strstr(body, "state=off")) web_led_set(false);

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /ota POST ---------------------------------------------------------

static esp_err_t handle_ota_post(httpd_req_t *req) {
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    static char buf[1024];
    int remaining = req->content_len;
    bool first_chunk = true;

    // multipart/form-data: skip past the part headers (\r\n\r\n) before writing
    bool header_skipped = false;
    static char header_buf[512];
    int header_pos = 0;

    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        remaining -= received;

        char *data = buf;
        int data_len = received;

        if (!header_skipped) {
            int old_pos = header_pos;
            int copy = received < (int)(sizeof(header_buf) - header_pos - 1)
                       ? received : (int)(sizeof(header_buf) - header_pos - 1);
            memcpy(header_buf + header_pos, buf, copy);
            header_pos += copy;
            header_buf[header_pos] = '\0';

            char *sep = strstr(header_buf, "\r\n\r\n");
            if (sep) {
                header_skipped = true;
                // skip = total bytes to consume from start of multipart body
                // old_pos = bytes already consumed in previous chunks
                // so buf_skip = bytes to skip in the current buf
                int skip = (sep + 4) - header_buf;
                int buf_skip = skip - old_pos;
                if (buf_skip < 0) buf_skip = 0;
                data = buf + buf_skip;
                data_len = received - buf_skip;
            } else {
                continue;
            }
        }

        if (data_len <= 0) continue;

        if (first_chunk) {
            first_chunk = false;
            if (data_len >= 1 && (uint8_t)data[0] != 0xE9) {
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a valid firmware image");
                return ESP_FAIL;
            }
        }

        err = esp_ota_write(ota_handle, data, data_len);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
    }

    if (esp_ota_end(ota_handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(update_part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ---- /ota/verify POST --------------------------------------------------

static esp_err_t handle_ota_verify(httpd_req_t *req) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA verify: %s", err == ESP_OK ? "marked valid" : "already valid");
    const char *msg = (err == ESP_OK)
        ? "Firmware marked valid. Rollback cancelled."
        : "Already valid or rollback not enabled.";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

// ---- /settings GET -----------------------------------------------------

static esp_err_t handle_settings_get(httpd_req_t *req) {
    settings_t cfg;
    settings_get(&cfg);
    static char buf[320];
    settings_encode(&cfg, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/x-www-form-urlencoded");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

// ---- /settings POST ----------------------------------------------------

static esp_err_t handle_settings_post(httpd_req_t *req) {
    char body[384] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    // Check query string for fwd=0 (peer-forwarded, don't re-forward)
    char query[32] = {0};
    bool do_forward = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (strstr(query, "fwd=0")) do_forward = false;
    }

    settings_t cfg;
    if (!settings_decode(body, &cfg)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad settings body");
        return ESP_FAIL;
    }

    if (do_forward) {
        settings_apply_and_forward(&cfg);
    } else {
        settings_apply_local(&cfg);
    }

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /node_config GET --------------------------------------------------

static esp_err_t handle_node_config_get(httpd_req_t *req) {
    static char buf[32];
    int len = snprintf(buf, sizeof(buf), "{\"num_leds\":%u}", node_config_get_num_leds());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// ---- /node_config POST -------------------------------------------------

static esp_err_t handle_node_config_post(httpd_req_t *req) {
    char body[32] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    char *p = strstr(body, "num_leds=");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing num_leds");
        return ESP_FAIL;
    }
    uint16_t n = (uint16_t)strtoul(p + 9, NULL, 10);
    node_config_save_num_leds(n);
    led_set_count(node_config_get_num_leds()); // apply immediately (clamped)

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /led_pixel POST ---------------------------------------------------

static esp_err_t handle_led_pixel_post(httpd_req_t *req) {
    char body[32] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    char *p = strstr(body, "idx=");
    int idx = p ? (int)strtol(p + 4, NULL, 10) : -1;
    led_set_pixel(idx);

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /pixel_layout GET -------------------------------------------------

static esp_err_t handle_pixel_layout_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    FILE *f = fopen("/spiffs/pixel_layout.csv", "r");
    if (!f) {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }
    static char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---- /pixel_layout POST ------------------------------------------------

static esp_err_t handle_pixel_layout_post(httpd_req_t *req) {
    FILE *f = fopen("/spiffs/pixel_layout.csv", "w");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPIFFS open failed");
        return ESP_FAIL;
    }

    static char buf[512];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            fclose(f);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        fwrite(buf, 1, received, f);
        remaining -= received;
    }
    fclose(f);

    pixel_layout_load();

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- preset peer forwarding --------------------------------------------

/* POST body to /presets/<sub>?fwd=0 on every discovered peer. */
static void preset_forward(const char *sub, const char *body) {
    peer_t peers[MAX_PEERS];
    int n = discovery_get_peers(peers, MAX_PEERS);
    static char url[80];
    for (int i = 0; i < n; i++) {
        snprintf(url, sizeof(url), "http://%s/presets/%s?fwd=0", peers[i].ip, sub);
        esp_http_client_config_t cfg = {
            .url             = url,
            .method          = HTTP_METHOD_POST,
            .timeout_ms      = SETTINGS_HTTP_TIMEOUT_MS,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_http_client_set_header(client, "Content-Type",
                                   "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "preset fwd %s to %s failed: %s",
                     sub, peers[i].ip, esp_err_to_name(err));
        esp_http_client_cleanup(client);
    }
}

/* Decode URL-encoded name= field from body into out (null-terminated). */
static void decode_name_field(const char *body, char *out, int out_size) {
    out[0] = '\0';
    const char *p = strstr(body, "name=");
    if (!p || !p[5]) return;
    const char *src = p + 5;
    int ni = 0;
    while (*src && ni < out_size - 1) {
        if (*src == '+') { out[ni++] = ' '; src++; }
        else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            out[ni++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '&') { break; }
        else { out[ni++] = *src++; }
    }
    out[ni] = '\0';
}

// ---- /presets GET ------------------------------------------------------

static esp_err_t handle_presets_get(httpd_req_t *req) {
    static char buf[1024];
    int len = presets_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// ---- /presets/save POST ------------------------------------------------
// Saves current settings under name on this node, then forwards to peers.
// Peers receive the encoded settings in the body so they store the same data
// regardless of which node they are (body: name=<n>&settings=<enc>).

static esp_err_t handle_presets_save(httpd_req_t *req) {
    char body[640] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    char name[PRESET_NAME_MAX] = {0};
    decode_name_field(body, name, sizeof(name));
    if (!name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    char query[32] = {0};
    bool do_forward = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        if (strstr(query, "fwd=0")) do_forward = false;

    /* Check if peer provided pre-encoded settings (fwd=0 path) */
    char *sp = strstr(body, "&settings=");
    settings_t cfg;
    if (sp) {
        /* Peer-forwarded: decode the embedded settings string */
        settings_get(&cfg);
        settings_decode(sp + 10, &cfg);
    } else {
        /* Original request: use current local settings */
        settings_get(&cfg);
    }

    if (!presets_save(name, &cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed (full?)");
        return ESP_FAIL;
    }

    if (do_forward) {
        /* Build forward body: name=<n>&settings=<enc> */
        static char fwd_body[640];
        static char enc[512];
        settings_encode(&cfg, enc, sizeof(enc));
        snprintf(fwd_body, sizeof(fwd_body), "name=%s&settings=%s",
                 strstr(body, "name=") + 5,   /* already URL-encoded name */
                 enc);
        preset_forward("save", fwd_body);
    }

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /presets/load POST ------------------------------------------------

static esp_err_t handle_presets_load(httpd_req_t *req) {
    char body[80] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    char name[PRESET_NAME_MAX] = {0};
    decode_name_field(body, name, sizeof(name));
    if (!name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    settings_t cfg;
    if (!presets_load(name, &cfg)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Preset not found");
        return ESP_FAIL;
    }
    /* settings_apply_and_forward already pushes to all peers */
    settings_apply_and_forward(&cfg);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /presets/delete POST ----------------------------------------------

static esp_err_t handle_presets_delete(httpd_req_t *req) {
    char body[80] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    char name[PRESET_NAME_MAX] = {0};
    decode_name_field(body, name, sizeof(name));
    if (!name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    char query[32] = {0};
    bool do_forward = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        if (strstr(query, "fwd=0")) do_forward = false;

    presets_delete(name);
    if (do_forward) preset_forward("delete", strstr(body, "name="));

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /presets/default POST ---------------------------------------------

static esp_err_t handle_presets_set_default(httpd_req_t *req) {
    char body[80] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    char name[PRESET_NAME_MAX] = {0};
    decode_name_field(body, name, sizeof(name));  /* empty = clear default */

    char query[32] = {0};
    bool do_forward = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        if (strstr(query, "fwd=0")) do_forward = false;

    presets_set_default(name);
    if (do_forward) {
        const char *fwd_body = strstr(body, "name=");
        preset_forward("default", fwd_body ? fwd_body : "name=");
    }

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- start -------------------------------------------------------------

void web_server_start(void) {
    settings_start_flash_task();

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 10;
    config.max_uri_handlers  = 18;
    config.max_open_sockets  = 5;    // leave room for discovery/time_sync/forward_task sockets
    config.lru_purge_enable  = true; // recycle oldest idle socket when at max_open_sockets

    ESP_LOGI(TAG, "Starting httpd...");
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }
    ESP_LOGI(TAG, "httpd started");

    httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = handle_get        },
        { .uri = "/state",      .method = HTTP_GET,  .handler = handle_state      },
        { .uri = "/led",        .method = HTTP_POST, .handler = handle_led_post   },
        { .uri = "/ota",        .method = HTTP_POST, .handler = handle_ota_post   },
        { .uri = "/ota/verify", .method = HTTP_POST, .handler = handle_ota_verify },
        { .uri = "/settings",     .method = HTTP_GET,  .handler = handle_settings_get     },
        { .uri = "/settings",     .method = HTTP_POST, .handler = handle_settings_post    },
        { .uri = "/node_config",  .method = HTTP_GET,  .handler = handle_node_config_get  },
        { .uri = "/node_config",  .method = HTTP_POST, .handler = handle_node_config_post },
        { .uri = "/pixel_layout",   .method = HTTP_GET,  .handler = handle_pixel_layout_get   },
        { .uri = "/pixel_layout",   .method = HTTP_POST, .handler = handle_pixel_layout_post  },
        { .uri = "/led_pixel",        .method = HTTP_POST, .handler = handle_led_pixel_post        },
        { .uri = "/presets",          .method = HTTP_GET,  .handler = handle_presets_get           },
        { .uri = "/presets/save",     .method = HTTP_POST, .handler = handle_presets_save          },
        { .uri = "/presets/load",     .method = HTTP_POST, .handler = handle_presets_load          },
        { .uri = "/presets/delete",   .method = HTTP_POST, .handler = handle_presets_delete        },
        { .uri = "/presets/default",  .method = HTTP_POST, .handler = handle_presets_set_default   },
    };
    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
