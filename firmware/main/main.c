#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include "http_server.h"
#include "logger.h"
#include "sensors.h"
#include "wifi_ap.h"

static const char *TAG = "ember";

void app_main(void)
{
    ESP_LOGI(TAG, "ember boot");

    ESP_ERROR_CHECK(sensors_init());

    QueueHandle_t log_queue = NULL;
    ESP_ERROR_CHECK(logger_start(&log_queue));
    ESP_ERROR_CHECK(sensors_start(log_queue));

#if !CONFIG_EMBER_FAKE_SENSORS
    ESP_ERROR_CHECK(wifi_ap_start());
    ESP_ERROR_CHECK(http_server_start());
#else
    ESP_LOGW(TAG, "wifi/httpd skipped (synthetic sensors)");
#endif
}
