#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "logger.h"
#include "sensors.h"

static const char *TAG = "ember";

void app_main(void)
{
    ESP_LOGI(TAG, "ember boot");

    ESP_ERROR_CHECK(sensors_init());

    QueueHandle_t log_queue = NULL;
    ESP_ERROR_CHECK(logger_start(&log_queue));
    ESP_ERROR_CHECK(sensors_start(log_queue));
}
