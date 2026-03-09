#include "web_server.h"
#include "discovery.h"
#include "node_config.h"
#include "grid_config.h"
#include "time_sync.h"
#include "settings_sync.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "led.h"
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

    static char buf[1024];
    int pos = 0;

#define A(...) pos += snprintf(buf + pos, sizeof(buf) - pos, __VA_ARGS__)

    settings_t cfg;
    settings_get(&cfg);

    A("{\"led\":%s,\"sync_ms\":%llu,\"ota_pending\":%s"
      ",\"ts_role\":\"%s\",\"ts_offset_us\":%lld,\"ts_rtt_us\":%lld"
      ",\"ts_sync_count\":%lu,\"ts_fail_count\":%lu"
      ",\"flash_enabled\":%s,\"period_ms\":%lu,\"duty_percent\":%u"
      ",\"r\":%u,\"g\":%u,\"b\":%u"
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
      (unsigned)cfg.r, (unsigned)cfg.g, (unsigned)cfg.b);

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
    static char buf[64];
    settings_encode(&cfg, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/x-www-form-urlencoded");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

// ---- /settings POST ----------------------------------------------------

static esp_err_t handle_settings_post(httpd_req_t *req) {
    char body[128] = {0};
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

// ---- /grid_config GET --------------------------------------------------

static esp_err_t handle_grid_config_get(httpd_req_t *req) {
    grid_config_t g;
    grid_config_get(&g);
    static char buf[64];
    int len = snprintf(buf, sizeof(buf),
        "{\"rows\":%u,\"cols\":%u,\"origin\":%u,\"row_first\":%u}",
        (unsigned)g.rows, (unsigned)g.cols,
        (unsigned)g.origin, (unsigned)g.row_first);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// ---- /grid_config POST -------------------------------------------------

static esp_err_t handle_grid_config_post(httpd_req_t *req) {
    char body[64] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) httpd_req_recv(req, body, recv_len);

    grid_config_t g;
    grid_config_get(&g);  // start from current values

    char *p;
    if ((p = strstr(body, "rows=")))      g.rows      = (uint8_t)strtoul(p + 5, NULL, 10);
    if ((p = strstr(body, "cols=")))      g.cols      = (uint8_t)strtoul(p + 5, NULL, 10);
    if ((p = strstr(body, "origin=")))    g.origin    = (uint8_t)strtoul(p + 7, NULL, 10);
    if ((p = strstr(body, "row_first="))) g.row_first = (uint8_t)strtoul(p + 10, NULL, 10);

    grid_config_save(&g);

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
    config.max_uri_handlers  = 15;
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
        { .uri = "/grid_config",  .method = HTTP_GET,  .handler = handle_grid_config_get  },
        { .uri = "/grid_config",  .method = HTTP_POST, .handler = handle_grid_config_post },
        { .uri = "/led_pixel",    .method = HTTP_POST, .handler = handle_led_pixel_post   },
    };
    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
