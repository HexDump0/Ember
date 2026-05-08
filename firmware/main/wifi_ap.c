#include "wifi_ap.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#define AP_CHANNEL       1
#define AP_MAX_CONN      4

static const char *TAG = "wifi_ap";

static void on_ap_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client joined " MACSTR " aid=%d",
                 MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "client left " MACSTR " aid=%d",
                 MAC2STR(e->mac), e->aid);
    }
}

esp_err_t wifi_ap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_ap_event, NULL, NULL));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));

    wifi_config_t wc = {0};
    int n = snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid),
                     "ember-%02x%02x%02x", mac[3], mac[4], mac[5]);
    wc.ap.ssid_len       = (uint8_t)n;
    wc.ap.channel        = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.authmode       = WIFI_AUTH_OPEN;
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "softap up: ssid=%s ch=%d", wc.ap.ssid, AP_CHANNEL);
    return ESP_OK;
}
