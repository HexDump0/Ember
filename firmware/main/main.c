#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensors.h"

static const char *TAG = "ember";

#define SAMPLE_PERIOD_MS 100

static void sample_task(void *arg)
{
    (void)arg;

    TickType_t next = xTaskGetTickCount();
    for (;;) {
        ember_sample_t s;
        esp_err_t err = sensors_read(&s);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "t=%lu ms  a=[%6d %6d %6d]  g=[%6d %6d %6d]  "
                     "p=%ld Pa  T=%d.%02d C",
                     (unsigned long)s.t_ms,
                     s.ax, s.ay, s.az,
                     s.gx, s.gy, s.gz,
                     (long)s.pressure_pa,
                     s.temp_c100 / 100, abs(s.temp_c100) % 100);
        } else {
            ESP_LOGW(TAG, "sensor read failed: %s", esp_err_to_name(err));
        }
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ember boot");

    ESP_ERROR_CHECK(sensors_init());

    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);
}
