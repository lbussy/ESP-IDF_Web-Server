/**
 * @file main.cpp
 * @brief Minimal example application using the http_server component.
 *
 * This example starts a Wi-Fi SoftAP so the device is reachable without
 * external infrastructure. It then starts the HTTP server and registers a
 * small custom API endpoint.
 */

#include <cstring>

extern "C"
{
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
}

#include "http_server.hpp"

namespace
{
static const char *TAG = "http_server_basic";

static esp_err_t init_nvs()
{
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        (void)nvs_flash_erase();
        rc = nvs_flash_init();
    }
    return rc;
}

static esp_err_t init_netif_and_event_loop()
{
    esp_err_t rc = esp_netif_init();
    if (rc != ESP_OK)
    {
        return rc;
    }

    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE)
    {
        return rc;
    }

    return ESP_OK;
}

static esp_err_t start_softap()
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == nullptr)
    {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t rc = esp_wifi_init(&cfg);
    if (rc != ESP_OK)
    {
        return rc;
    }

    rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (rc != ESP_OK)
    {
        return rc;
    }

    wifi_config_t ap_cfg{};
    std::strncpy(reinterpret_cast<char *>(ap_cfg.ap.ssid), "http-server", sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = std::strlen(reinterpret_cast<const char *>(ap_cfg.ap.ssid));
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    rc = esp_wifi_set_mode(WIFI_MODE_AP);
    if (rc != ESP_OK)
    {
        return rc;
    }

    rc = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (rc != ESP_OK)
    {
        return rc;
    }

    rc = esp_wifi_start();
    if (rc != ESP_OK)
    {
        return rc;
    }

    ESP_LOGI(TAG, "Wi-Fi SoftAP started with SSID '%s'.", ap_cfg.ap.ssid);
    return ESP_OK;
}

static esp_err_t handle_ping(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "pong\n", HTTPD_RESP_USE_STRLEN);
}
} // namespace

extern "C" void app_main(void)
{
    esp_err_t rc = init_nvs();
    if (rc != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed: %s.", esp_err_to_name(rc));
        return;
    }

    rc = init_netif_and_event_loop();
    if (rc != ESP_OK)
    {
        ESP_LOGE(TAG, "Network init failed: %s.", esp_err_to_name(rc));
        return;
    }

    rc = start_softap();
    if (rc != ESP_OK)
    {
        ESP_LOGE(TAG, "SoftAP start failed: %s.", esp_err_to_name(rc));
        return;
    }

    http_srv::start();

    rc = http_srv::wait_until_running(pdMS_TO_TICKS(2000));
    if (rc != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP server did not become ready: %s.", esp_err_to_name(rc));
        http_srv::stop();
        return;
    }

    rc = http_srv::register_uri("/api/ping", HTTP_GET, handle_ping);
    if (rc != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register /api/ping: %s.", esp_err_to_name(rc));
    }

    ESP_LOGI(TAG, "HTTP server is running.");
    ESP_LOGI(TAG, "Open http://192.168.4.1/ and http://192.168.4.1/api/ping.");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
