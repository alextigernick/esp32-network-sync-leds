#include "web_server.h"
#include "discovery.h"
#include "time_sync.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"

#define TAG "web_server"

static bool s_led_on = false;

static void led_set(bool on) {
    s_led_on = on;
    gpio_set_level(LED_GPIO, on ? 1 : 0);
}

// ---- HTML page ---------------------------------------------------------

// Writes the full page into buf (max buf_size). Returns bytes written.
static int build_page(char *buf, size_t buf_size) {
    peer_t peers[MAX_PEERS];
    int peer_count = discovery_get_peers(peers, MAX_PEERS);
    uint64_t sync_ms = time_sync_get_ms();

    int pos = 0;

#define APPEND(...) pos += snprintf(buf + pos, buf_size - pos, __VA_ARGS__)

    APPEND(
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Control</title>"
        "<style>"
        "  body{font-family:monospace;background:#111;color:#eee;display:flex;"
        "       flex-direction:column;align-items:center;padding:2rem;gap:1.5rem;}"
        "  h1{margin:0;font-size:1.4rem;letter-spacing:.1em;color:#0f9;}"
        "  .card{background:#1e1e1e;border:1px solid #333;border-radius:8px;"
        "        padding:1.2rem 1.8rem;width:280px;}"
        "  .card h2{margin:0 0 1rem;font-size:.9rem;text-transform:uppercase;"
        "           color:#888;letter-spacing:.08em;}"
        "  button{width:100%%;padding:.8rem;border:none;border-radius:6px;"
        "         font-size:1rem;cursor:pointer;font-family:monospace;"
        "         transition:background .15s;}"
        "  .on {background:#0f9;color:#000;}"
        "  .off{background:#333;color:#aaa;}"
        "  select{width:100%%;padding:.6rem;background:#282828;color:#eee;"
        "         border:1px solid #444;border-radius:6px;font-family:monospace;"
        "         font-size:.9rem;cursor:pointer;}"
        "  .time{font-size:.8rem;color:#555;text-align:center;}"
        "</style></head><body>"
    );

    APPEND("<h1>ESP32 NODE</h1>");

    // LED control card
    APPEND(
        "<div class='card'>"
        "<h2>LED Control</h2>"
        "<form method='POST' action='/led'>"
        "<button class='%s' name='state' value='%s'>LED is %s — tap to turn %s</button>"
        "</form></div>",
        s_led_on ? "on" : "off",
        s_led_on ? "off" : "on",
        s_led_on ? "ON" : "OFF",
        s_led_on ? "OFF" : "ON"
    );

    // Peer navigator card
    APPEND(
        "<div class='card'>"
        "<h2>Other Nodes (%d found)</h2>", peer_count
    );

    if (peer_count == 0) {
        APPEND("<select disabled><option>No peers yet...</option></select>");
    } else {
        APPEND("<select onchange=\"if(this.value)window.location='http://'+this.value\">"
               "<option value=''>-- navigate to node --</option>");
        for (int i = 0; i < peer_count; i++) {
            APPEND("<option value='%s'>%s  (%s)</option>",
                   peers[i].ip, peers[i].name, peers[i].ip);
        }
        APPEND("</select>");
    }

    APPEND("</div>");

    // Sync time display
    APPEND("<p class='time'>sync time: %llu ms</p>", (unsigned long long)sync_ms);

    APPEND("</body></html>");

#undef APPEND
    return pos;
}

// ---- HTTP handlers -----------------------------------------------------

static esp_err_t handle_get(httpd_req_t *req) {
    static char buf[4096];
    int len = build_page(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t handle_led_post(httpd_req_t *req) {
    char body[64] = {0};
    int recv_len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    if (recv_len > 0) {
        httpd_req_recv(req, body, recv_len);
    }

    // Parse "state=on" or "state=off"
    if (strstr(body, "state=on")) {
        led_set(true);
    } else if (strstr(body, "state=off")) {
        led_set(false);
    }

    // Redirect back to main page
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- start -------------------------------------------------------------

void web_server_start(void) {
    ESP_LOGI(TAG, "Configuring LED GPIO %d", LED_GPIO);
    // Configure LED GPIO
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    ESP_LOGI(TAG, "gpio_config ok");
    led_set(false);
    ESP_LOGI(TAG, "led_set ok");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting httpd...");
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }
    ESP_LOGI(TAG, "httpd started");

    httpd_uri_t uri_get = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_get,
    };
    httpd_register_uri_handler(server, &uri_get);

    httpd_uri_t uri_led = {
        .uri     = "/led",
        .method  = HTTP_POST,
        .handler = handle_led_post,
    };
    httpd_register_uri_handler(server, &uri_led);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
