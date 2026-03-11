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
 *       "ts_role": "root"|"follower"|"elected"|"->x.x.x.x",
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
#include "renderer.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "led.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "web_server"

extern const uint8_t web_ui_html_start[]    asm("_binary_web_ui_html_start");
extern const uint8_t web_ui_html_end[]      asm("_binary_web_ui_html_end");
extern const uint8_t calibrate_html_start[] asm("_binary_calibrate_html_start");
extern const uint8_t calibrate_html_end[]   asm("_binary_calibrate_html_end");
extern const uint8_t server_crt_start[]     asm("_binary_server_crt_start");
extern const uint8_t server_crt_end[]       asm("_binary_server_crt_end");
extern const uint8_t server_key_start[]     asm("_binary_server_key_start");
extern const uint8_t server_key_end[]       asm("_binary_server_key_end");

static bool s_led_on = false;

// Allow the calibration page (served from any device) to make cross-origin
// requests to peer devices.  Added to every response that the calibration
// tool needs to read.
static void add_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static void web_led_set(bool on) {
    s_led_on = on;
    led_set(on);
}

// ---- /state JSON endpoint ----------------------------------------------

static esp_err_t handle_state(httpd_req_t *req) {
    add_cors(req);
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

    static char buf[1536];
    int pos = 0;

#define A(...) pos += snprintf(buf + pos, sizeof(buf) - pos, __VA_ARGS__)

    settings_t cfg;
    settings_get(&cfg);

    uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t free_heap        = esp_get_free_heap_size();
    uint32_t frame_count      = renderer_get_frame_count();
    uint32_t hwm_render       = renderer_get_stack_hwm();
    uint32_t hwm_fwd          = settings_get_fwd_stack_hwm();
    uint32_t hwm_web          = uxTaskGetStackHighWaterMark(NULL);
    uint32_t hwm_time         = time_sync_get_stack_hwm();
    uint32_t hwm_disc_listen  = discovery_get_listen_stack_hwm();
    uint32_t hwm_disc_ann     = discovery_get_announce_stack_hwm();
    A("{\"led\":%s,\"sync_ms\":%llu,\"ota_pending\":%s"
      ",\"ts_role\":\"%s\",\"ts_offset_us\":%lld,\"ts_rtt_us\":%lld"
      ",\"ts_sync_count\":%lu,\"ts_fail_count\":%lu,\"uptime_ms\":%lu"
      ",\"free_heap\":%lu,\"frame_count\":%lu"
      ",\"hwm_render\":%lu,\"hwm_fwd\":%lu,\"hwm_web\":%lu"
      ",\"hwm_time\":%lu,\"hwm_disc_listen\":%lu,\"hwm_disc_ann\":%lu"
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
      (unsigned long)uptime_ms,
      (unsigned long)free_heap, (unsigned long)frame_count,
      (unsigned long)hwm_render, (unsigned long)hwm_fwd, (unsigned long)hwm_web,
      (unsigned long)hwm_time, (unsigned long)hwm_disc_listen, (unsigned long)hwm_disc_ann,
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

// ---- /calibrate HTML page ----------------------------------------------

static esp_err_t handle_calibrate_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)calibrate_html_start,
                    calibrate_html_end - calibrate_html_start);
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
    // Blank LEDs for the entire OTA write to reduce power draw and avoid glitches.
    // flash_task would otherwise keep overwriting any single-frame blackout.
    renderer_set_ota_blackout(true);

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
    add_cors(req);
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
    add_cors(req);
    strip_cfg_t strips[MAX_STRIPS];
    int strip_count;
    node_config_get_strips(strips, &strip_count);

    static char buf[320];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"num_leds\":%u,\"max_bright\":%u,\"ct_bias\":%d"
                    ",\"layout_x\":%.2f,\"layout_y\":%.2f,\"layout_rot\":%.2f"
                    ",\"strips\":[",
                    node_config_get_num_leds(), node_config_get_max_bright(),
                    (int)node_config_get_ct_bias(),
                    node_config_get_layout_x_offset(),
                    node_config_get_layout_y_offset(),
                    node_config_get_layout_rotation());
    for (int i = 0; i < strip_count; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"gpio\":%u,\"num_leds\":%u}",
                        i > 0 ? "," : "", strips[i].gpio, strips[i].num_leds);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

// ---- /node_config POST -------------------------------------------------

static esp_err_t handle_node_config_post(httpd_req_t *req) {
    add_cors(req);
    char body[256] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    bool changed = false;

    // Per-strip config: strip_gpio_N and strip_leds_N
    for (int i = 0; i < MAX_STRIPS; i++) {
        char key_gpio[16], key_leds[16];
        snprintf(key_gpio, sizeof(key_gpio), "strip_gpio_%d=", i);
        snprintf(key_leds, sizeof(key_leds), "strip_leds_%d=", i);

        char *pg = strstr(body, key_gpio);
        char *pl = strstr(body, key_leds);
        if (pg || pl) {
            strip_cfg_t strips[MAX_STRIPS];
            int count;
            node_config_get_strips(strips, &count);
            if (pg) strips[i].gpio     = (uint8_t)strtoul(pg + strlen(key_gpio), NULL, 10);
            if (pl) strips[i].num_leds = (uint16_t)strtoul(pl + strlen(key_leds), NULL, 10);
            node_config_save_strip(i, strips[i].gpio, strips[i].num_leds);
            changed = true;
        }
    }

    char *p = strstr(body, "max_bright=");
    if (p) {
        uint8_t v = (uint8_t)strtoul(p + 11, NULL, 10);
        node_config_save_max_bright(v);
        changed = true;
    }
    p = strstr(body, "ct_bias=");
    if (p) {
        int8_t v = (int8_t)strtol(p + 8, NULL, 10);
        node_config_save_ct_bias(v);
        changed = true;
    }

    bool layout_transform_changed = false;
    char *px = strstr(body, "layout_x=");
    char *py = strstr(body, "layout_y=");
    char *pr = strstr(body, "layout_rot=");
    if (px || py || pr) {
        float x   = node_config_get_layout_x_offset();
        float y   = node_config_get_layout_y_offset();
        float rot = node_config_get_layout_rotation();
        if (px) x   = strtof(px + 9,   NULL);
        if (py) y   = strtof(py + 9,   NULL);
        if (pr) rot = strtof(pr + 11,  NULL);
        node_config_save_layout_transform(x, y, rot);
        pixel_layout_load();
        changed = true;
        layout_transform_changed = true;
    }
    (void)layout_transform_changed;

    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing node config field");
        return ESP_FAIL;
    }

    // Re-initialize LED strips if strip config changed
    if (strstr(body, "strip_gpio_") || strstr(body, "strip_leds_"))
        led_reinit();

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /identify POST ----------------------------------------------------

static esp_err_t handle_identify(httpd_req_t *req) {
    renderer_identify(3000);  // flash white for 3 s on this node only
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /led_pixel POST ---------------------------------------------------

static esp_err_t handle_led_pixel_post(httpd_req_t *req) {
    add_cors(req);
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
    add_cors(req);
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

// ---- /presets/clear POST -----------------------------------------------

static esp_err_t handle_presets_clear(httpd_req_t *req) {
    char query[32] = {0};
    bool do_forward = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        if (strstr(query, "fwd=0")) do_forward = false;

    presets_clear();
    if (do_forward) preset_forward("clear", "");

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /presets/sync POST ------------------------------------------------
// Clears all peers' presets then pushes every local preset to each peer.

static esp_err_t handle_presets_sync(httpd_req_t *req) {
    peer_t peers[MAX_PEERS];
    int n_peers = discovery_get_peers(peers, MAX_PEERS);

    /* 1. Clear all presets on each peer */
    static char url[80];
    for (int p = 0; p < n_peers; p++) {
        snprintf(url, sizeof(url), "http://%s/presets/clear?fwd=0", peers[p].ip);
        esp_http_client_config_t cfg = {
            .url        = url,
            .method     = HTTP_METHOD_POST,
            .timeout_ms = SETTINGS_HTTP_TIMEOUT_MS,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_http_client_set_header(client, "Content-Type",
                                   "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, "", 0);
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "sync clear to %s failed: %s", peers[p].ip, esp_err_to_name(err));
        esp_http_client_cleanup(client);
    }

    /* 2. Push each local preset to all peers */
    preset_info_t infos[PRESETS_MAX];
    int cnt = presets_list(infos, PRESETS_MAX);

    static char fwd_body[680];
    static char enc[512];
    for (int i = 0; i < cnt; i++) {
        settings_t cfg;
        if (!presets_load(infos[i].name, &cfg)) continue;
        settings_encode(&cfg, enc, sizeof(enc));

        /* URL-encode name inline — only alphanumerics and spaces are typical */
        static char name_enc[PRESET_NAME_MAX * 3];
        int ni = 0;
        for (const char *s = infos[i].name; *s && ni < (int)sizeof(name_enc) - 4; s++) {
            if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.') {
                name_enc[ni++] = *s;
            } else if (*s == ' ') {
                name_enc[ni++] = '+';
            } else {
                ni += snprintf(name_enc + ni, sizeof(name_enc) - ni, "%%%02X",
                               (unsigned char)*s);
            }
        }
        name_enc[ni] = '\0';

        snprintf(fwd_body, sizeof(fwd_body), "name=%s&settings=%s", name_enc, enc);

        for (int p = 0; p < n_peers; p++) {
            snprintf(url, sizeof(url), "http://%s/presets/save?fwd=0", peers[p].ip);
            esp_http_client_config_t hcfg = {
                .url        = url,
                .method     = HTTP_METHOD_POST,
                .timeout_ms = SETTINGS_HTTP_TIMEOUT_MS,
            };
            esp_http_client_handle_t client = esp_http_client_init(&hcfg);
            esp_http_client_set_header(client, "Content-Type",
                                       "application/x-www-form-urlencoded");
            esp_http_client_set_post_field(client, fwd_body, (int)strlen(fwd_body));
            esp_err_t err = esp_http_client_perform(client);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "sync save '%s' to %s failed: %s",
                         infos[i].name, peers[p].ip, esp_err_to_name(err));
            esp_http_client_cleanup(client);
        }
    }

    /* 3. Forward the boot-default designation */
    char def_name[PRESET_NAME_MAX] = {0};
    presets_get_default(def_name, sizeof(def_name));
    /* URL-encode the default name the same way */
    static char def_enc[PRESET_NAME_MAX * 3 + 6]; /* "name=" + encoded + NUL */
    int di = 0;
    di += snprintf(def_enc, sizeof(def_enc), "name=");
    for (const char *s = def_name; *s && di < (int)sizeof(def_enc) - 4; s++) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
            (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.') {
            def_enc[di++] = *s;
        } else if (*s == ' ') {
            def_enc[di++] = '+';
        } else {
            di += snprintf(def_enc + di, sizeof(def_enc) - di, "%%%02X",
                           (unsigned char)*s);
        }
    }
    def_enc[di] = '\0';
    for (int p = 0; p < n_peers; p++) {
        snprintf(url, sizeof(url), "http://%s/presets/default?fwd=0", peers[p].ip);
        esp_http_client_config_t dcfg = {
            .url        = url,
            .method     = HTTP_METHOD_POST,
            .timeout_ms = SETTINGS_HTTP_TIMEOUT_MS,
        };
        esp_http_client_handle_t client = esp_http_client_init(&dcfg);
        esp_http_client_set_header(client, "Content-Type",
                                   "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, def_enc, (int)strlen(def_enc));
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "sync default to %s failed: %s", peers[p].ip, esp_err_to_name(err));
        esp_http_client_cleanup(client);
    }

    ESP_LOGI(TAG, "preset sync: pushed %d presets to %d peers", cnt, n_peers);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- /fwd/{peer_ip}{path} reverse proxy --------------------------------
// Forwards GET/POST requests from the HTTPS calibration page to peer devices
// over plain HTTP (internal LAN only). This keeps the calibration page on a
// single HTTPS origin (the AP at 192.168.4.1) so the browser never sees
// mixed-content or cross-origin issues.

static esp_err_t handle_proxy(httpd_req_t *req) {
    add_cors(req);

    // URI: /fwd/{peer_ip}{path}  e.g. /fwd/192.168.4.2/led_pixel
    const char *uri = req->uri;
    const char *peer_start = uri + 5; // skip "/fwd/"

    // Find the '/' that separates IP from path
    const char *path_start = strchr(peer_start, '/');
    if (!path_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad proxy URI");
        return ESP_FAIL;
    }

    char peer_ip[48] = {0};
    int ip_len = (int)(path_start - peer_start);
    if (ip_len < 1 || ip_len >= (int)sizeof(peer_ip)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad peer IP");
        return ESP_FAIL;
    }
    memcpy(peer_ip, peer_start, ip_len);

    char url[128] = {0};
    snprintf(url, sizeof(url), "http://%s%s", peer_ip, path_start);

    // Read request body (for POST)
    char req_body[512];
    int body_len = 0;
    if (req->content_len > 0) {
        body_len = req->content_len < (int)sizeof(req_body) - 1
                   ? req->content_len : (int)sizeof(req_body) - 1;
        httpd_req_recv(req, req_body, body_len);
        req_body[body_len] = '\0';
    }

    // Choose method
    esp_http_client_method_t method =
        (req->method == HTTP_POST) ? HTTP_METHOD_POST : HTTP_METHOD_GET;

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = method,
        .timeout_ms = SETTINGS_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Client init failed");
        return ESP_FAIL;
    }

    if (body_len > 0) {
        char ct[80] = {0};
        httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
        if (ct[0]) esp_http_client_set_header(client, "Content-Type", ct);
        // Note: do NOT call set_post_field here — it only works with perform().
        // With the open()/write()/fetch_headers() streaming API we write manually below.
    }

    // Open connection — sends request line + headers (+ Content-Length: body_len for POST)
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "proxy open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Proxy connect failed");
        return ESP_FAIL;
    }

    // Send POST body (set_post_field is ignored by the streaming API; must use write())
    if (body_len > 0) {
        if (esp_http_client_write(client, req_body, body_len) < 0) {
            ESP_LOGW(TAG, "proxy write %s failed", url);
            esp_http_client_cleanup(client);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Proxy write failed");
            return ESP_FAIL;
        }
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    // Map status to response string
    if (status == 204) {
        httpd_resp_set_status(req, "204 No Content");
    } else if (status != 200) {
        char status_buf[32];
        snprintf(status_buf, sizeof(status_buf), "%d Error", status);
        httpd_resp_set_status(req, status_buf);
    }

    // Stream response body in chunks
    char chunk[512];
    int total = 0;
    int n;
    (void)content_len;
    while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
        httpd_resp_send_chunk(req, chunk, n);
        total += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGD(TAG, "proxy %s → %d (%d bytes)", url, status, total);
    return ESP_OK;
}

// ---- start -------------------------------------------------------------

// Register all URI handlers on a given httpd instance.
static void register_routes(httpd_handle_t server) {
    httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = handle_get           },
        { .uri = "/calibrate",  .method = HTTP_GET,  .handler = handle_calibrate_get },
        { .uri = "/state",      .method = HTTP_GET,  .handler = handle_state      },
        { .uri = "/led",        .method = HTTP_POST, .handler = handle_led_post   },
        { .uri = "/ota",        .method = HTTP_POST, .handler = handle_ota_post   },
        { .uri = "/ota/verify", .method = HTTP_POST, .handler = handle_ota_verify },
        { .uri = "/settings",     .method = HTTP_GET,  .handler = handle_settings_get     },
        { .uri = "/settings",     .method = HTTP_POST, .handler = handle_settings_post    },
        { .uri = "/node_config",  .method = HTTP_GET,  .handler = handle_node_config_get  },
        { .uri = "/node_config",  .method = HTTP_POST, .handler = handle_node_config_post },
        { .uri = "/pixel_layout", .method = HTTP_GET,  .handler = handle_pixel_layout_get  },
        { .uri = "/pixel_layout", .method = HTTP_POST, .handler = handle_pixel_layout_post },
        { .uri = "/led_pixel",    .method = HTTP_POST, .handler = handle_led_pixel_post    },
        { .uri = "/identify",     .method = HTTP_POST, .handler = handle_identify          },
        { .uri = "/presets",          .method = HTTP_GET,  .handler = handle_presets_get           },
        { .uri = "/presets/save",     .method = HTTP_POST, .handler = handle_presets_save          },
        { .uri = "/presets/load",     .method = HTTP_POST, .handler = handle_presets_load          },
        { .uri = "/presets/delete",   .method = HTTP_POST, .handler = handle_presets_delete        },
        { .uri = "/presets/default",  .method = HTTP_POST, .handler = handle_presets_set_default   },
        { .uri = "/presets/clear",    .method = HTTP_POST, .handler = handle_presets_clear         },
        { .uri = "/presets/sync",     .method = HTTP_POST, .handler = handle_presets_sync          },
        // Proxy: forwards calibration tool requests to peer devices over HTTP.
        // Must use wildcard matching — set uri_match_fn = httpd_uri_match_wildcard.
        { .uri = "/fwd/*",  .method = HTTP_GET,  .handler = handle_proxy },
        { .uri = "/fwd/*",  .method = HTTP_POST, .handler = handle_proxy },
    };
    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

void web_server_start(void) {
    // ---- Plain HTTP on port 80 (normal UI, peer-to-peer settings forwarding) ----
    httpd_config_t http_cfg    = HTTPD_DEFAULT_CONFIG();
    http_cfg.stack_size        = 8192;
    http_cfg.recv_wait_timeout = 30;
    http_cfg.send_wait_timeout = 10;
    http_cfg.max_uri_handlers  = 24; // routes + 2 proxy wildcard entries
    http_cfg.max_open_sockets  = 4;
    http_cfg.lru_purge_enable  = true;
    http_cfg.uri_match_fn      = httpd_uri_match_wildcard;

    httpd_handle_t http_server = NULL;
    if (httpd_start(&http_server, &http_cfg) == ESP_OK) {
        register_routes(http_server);
        ESP_LOGI(TAG, "HTTP  server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    // ---- HTTPS on port 443 (calibration tool — enables camera API in browsers) ----
    // Self-signed EC cert for 192.168.4.1 (the AP IP).  The calibration page is
    // always accessed from the AP; peer API calls are proxied via /fwd/{ip}{path}.
    httpd_ssl_config_t ssl_cfg        = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_cfg.httpd.stack_size          = 16384; // proxy handler runs esp_http_client inline
    ssl_cfg.httpd.recv_wait_timeout   = 30;
    ssl_cfg.httpd.send_wait_timeout   = 10;
    ssl_cfg.httpd.max_uri_handlers    = 24;
    ssl_cfg.httpd.max_open_sockets    = 3; // TLS sessions are memory-hungry; 3 is enough
    ssl_cfg.httpd.lru_purge_enable    = true;
    ssl_cfg.httpd.uri_match_fn        = httpd_uri_match_wildcard;
    ssl_cfg.servercert                = server_crt_start;
    ssl_cfg.servercert_len            = server_crt_end - server_crt_start;
    ssl_cfg.prvtkey_pem               = server_key_start;
    ssl_cfg.prvtkey_len               = server_key_end - server_key_start;

    httpd_handle_t https_server = NULL;
    if (httpd_ssl_start(&https_server, &ssl_cfg) == ESP_OK) {
        register_routes(https_server);
        ESP_LOGI(TAG, "HTTPS server started on port 443");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTPS server");
    }
}
