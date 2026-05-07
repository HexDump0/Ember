#include "logger.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensors.h"

#define QUEUE_DEPTH    64
#define TASK_STACK     4096
#define TASK_PRIO      4
#define REPORT_PERIOD  1000  /* ms */

static const char *TAG = "logger";

static QueueHandle_t s_queue;

static void logger_task(void *arg)
{
    (void)arg;

    uint32_t count       = 0;
    uint32_t total       = 0;
    uint32_t last_log_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (;;) {
        ember_sample_t s;
        if (xQueueReceive(s_queue, &s, pdMS_TO_TICKS(REPORT_PERIOD)) == pdTRUE) {
            count++;
            total++;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t dt     = now_ms - last_log_ms;
        if (dt >= REPORT_PERIOD) {
            UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
            ESP_LOGI(TAG,
                     "rx %lu samples in %lu ms (%lu Hz, total %lu, q=%u)",
                     (unsigned long)count, (unsigned long)dt,
                     (unsigned long)((count * 1000UL) / dt),
                     (unsigned long)total, (unsigned)depth);
            count       = 0;
            last_log_ms = now_ms;
        }
    }
}

esp_err_t logger_start(QueueHandle_t *out_queue)
{
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(ember_sample_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(logger_task, "logger",
                                TASK_STACK, NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (out_queue) {
        *out_queue = s_queue;
    }
    return ESP_OK;
}
