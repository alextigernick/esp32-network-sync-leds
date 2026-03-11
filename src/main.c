#include "config.h"
#include "discovery.h"
#include "node_config.h"
#include "pixel_layout.h"
#include "time_sync.h"
#include "presets.h"
#include "settings_sync.h"
#include "renderer.h"
#include "web_server.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"

#define TAG "main"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static char s_my_ip[16]   = {0};
static char s_my_name[32] = {0};

// Track connection attempts to detect when no AP is available
static int s_connect_attempts     = 0;
static int s_max_connect_attempts = WIFI_MAX_CONNECT_ATTEMPTS; // overridden per-node from MAC

static esp_timer_handle_t s_retry_timer = NULL;

static void retry_timer_cb(void *arg) {
    esp_wifi_connect();
}

// ---- WiFi event handler ------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connect_attempts++;
        if (s_connect_attempts < s_max_connect_attempts) {
            // Random inter-retry delay (500–2500 ms) so two nodes with the
            // same retry budget diverge and don't both become the AP.
            uint32_t delay_ms = WIFI_RETRY_DELAY_MIN_MS + (esp_random() % (WIFI_RETRY_DELAY_MAX_MS - WIFI_RETRY_DELAY_MIN_MS));
            ESP_LOGI(TAG, "Retry %d/%d in %lu ms", s_connect_attempts,
                     s_max_connect_attempts, delay_ms);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
        } else {
            ESP_LOGW(TAG, "No AP found after %d attempts", s_connect_attempts);
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_my_ip, sizeof(s_my_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_my_ip);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Station joined AP, AID=%d", ev->aid);
    }
}

// ---- common WiFi driver init -------------------------------------------

static void wifi_driver_init(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
}

// ---- promote to AP+STA -------------------------------------------------

static void become_ap(void) {
    ESP_LOGI(TAG, "No existing AP found — becoming the AP");

    esp_netif_create_default_wifi_ap();

    char ssid[33], pass[65];
    node_config_get_wifi_ssid(ssid, sizeof(ssid));
    node_config_get_wifi_pass(pass, sizeof(pass));

    wifi_config_t ap_cfg = { .ap = {
        .channel        = 1,
        .max_connection = 8,
        .authmode       = WIFI_AUTH_WPA2_PSK,
    }};
    strncpy((char *)ap_cfg.ap.ssid,     ssid, sizeof(ap_cfg.ap.ssid));
    strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    strncpy(s_my_ip, AP_IP, sizeof(s_my_ip) - 1);
    ESP_LOGI(TAG, "AP ready. SSID='%s'  IP=%s", ssid, s_my_ip);
}

// ---- try to join existing network, fall back to AP --------------------

static bool wifi_connect_or_become_ap(void) {
    esp_netif_create_default_wifi_sta();
    wifi_driver_init();

    char ssid[33], pass[65];
    node_config_get_wifi_ssid(ssid, sizeof(ssid));
    node_config_get_wifi_pass(pass, sizeof(pass));

    wifi_config_t sta_cfg = { .sta = {} };
    strncpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Scanning for '%s'...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           false, false, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        return true;  // joined as STA
    }

    // If this node is configured to never host the AP, keep retrying indefinitely.
    if (node_config_get_never_ap()) {
        ESP_LOGW(TAG, "never_ap set — retrying STA forever, not becoming AP");
        while (true) {
            s_connect_attempts = 0;
            xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
            esp_wifi_connect();
            bits = xEventGroupWaitBits(s_wifi_events,
                                       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                       false, false, portMAX_DELAY);
            if (bits & WIFI_CONNECTED_BIT) return true;
        }
    }

    // No AP found — stop STA and come up as AP instead
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
    // Unregister handlers so they don't fire during re-init
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler);

    // Re-init driver for AP mode
    wifi_driver_init();
    become_ap();
    return false;  // we are the AP
}

// ---- entry point -------------------------------------------------------

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t spiffs_ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (spiffs_ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s) — pixel layout unavailable",
                 esp_err_to_name(spiffs_ret));
    }

    node_config_load();
    // TEMPORARY: reset WiFi creds to compile-time defaults on every boot.
    // Remove once devices are accessible via web UI.
    // node_config_save_wifi_creds("", "");
    pixel_layout_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_events = xEventGroupCreate();

    esp_timer_create_args_t retry_timer_args = {
        .callback = retry_timer_cb,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &s_retry_timer));

    // Give each node a MAC-derived STA retry budget (2–9 attempts).
    // The node with the fewest retries fails STA first and becomes the AP;
    // others keep scanning and find it before exhausting their budget.
    // No boot delay — nodes start immediately and desync through retry timing.
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        s_max_connect_attempts = WIFI_RETRY_MIN + (int)(((uint32_t)mac[4] * 251 + mac[5]) % WIFI_RETRY_SPREAD);
        ESP_LOGI(TAG, "STA retry budget: %d attempts", s_max_connect_attempts);
    }

    bool is_sta = wifi_connect_or_become_ap();

    // Disable WiFi power save — prevents AP from buffering packets up to ~100ms
    // before delivering them to sleeping STAs. Critical for low-jitter time sync.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Build a unique node name from the last 3 bytes of the STA MAC
    {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(s_my_name, sizeof(s_my_name), "node-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
    }
    ESP_LOGI(TAG, "Node name: %s", s_my_name);

    // Init settings state before any task that might call settings_get()
    // (e.g. presets_apply_default via first_win_cb), then start renderer.
    settings_sync_init();
    renderer_start();

    if (is_sta) {
        time_sync_set_first_sync_cb(settings_fetch_from_peer);
        time_sync_set_first_win_cb(presets_apply_default);
        time_sync_start_elected(s_my_ip);
    } else {
        time_sync_start_root();
    }

    ESP_LOGI(TAG, "Starting discovery...");
    discovery_start(s_my_ip, s_my_name);
    ESP_LOGI(TAG, "Discovery started");

    ESP_LOGI(TAG, "Starting web server...");
    web_server_start();

    // AP node is always the time root — apply saved default preset immediately.
    // STA nodes defer this until they win election (via first_win_cb) or
    // fetch settings from the root peer on first time sync (via first_sync_cb).
    if (!is_sta) {
        presets_apply_default();
    }

    ESP_LOGI(TAG, "System ready. http://%s/", s_my_ip);
}
