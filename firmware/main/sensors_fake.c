#include "sensors.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SAMPLE_PERIOD_MS  10           /* 100 Hz */
#define TASK_STACK        4096
#define TASK_PRIO         6

#define ACCEL_1G          16384        /* MPU6050 ±2 g range LSB/g */
#define SEA_LEVEL_PA      101325
#define ASCENT_PA_PER_S   1200         /* ~100 m/s climb at sea level */

static const char *TAG = "sensors";

static SemaphoreHandle_t s_latest_lock;
static ember_sample_t    s_latest;
static bool              s_have_latest;

static QueueHandle_t s_log_queue;

esp_err_t sensors_init(void)
{
    s_latest_lock = xSemaphoreCreateMutex();
    if (s_latest_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "synthetic sensor backend active");
    return ESP_OK;
}

esp_err_t sensors_read(ember_sample_t *out)
{
    uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    float    t_s  = t_ms / 1000.0f;

    /* 1 Hz wobble around 1 g resting on Z, plus a 5 g burn between
       t=10 s and t=12 s so the trace is recognisable in the log. */
    float    burn  = (t_s >= 10.0f && t_s < 12.0f) ? 5.0f : 0.0f;
    float    wob   = 0.05f * sinf(2.0f * (float)M_PI * t_s);
    int16_t  ax    = (int16_t)(ACCEL_1G * wob);
    int16_t  ay    = (int16_t)(ACCEL_1G * wob * 0.5f);
    int16_t  az    = (int16_t)(ACCEL_1G * (1.0f + burn + wob));

    int16_t  gx    = (int16_t)(200 * sinf(2.0f * (float)M_PI * 0.5f * t_s));
    int16_t  gy    = (int16_t)(200 * cosf(2.0f * (float)M_PI * 0.5f * t_s));
    int16_t  gz    = 0;

    int32_t  pressure_pa = SEA_LEVEL_PA;
    if (t_s >= 10.0f) {
        float dt = t_s - 10.0f;
        pressure_pa -= (int32_t)(ASCENT_PA_PER_S * dt);
        if (pressure_pa < 30000) {
            pressure_pa = 30000;
        }
    }

    out->t_ms        = t_ms;
    out->ax          = ax;
    out->ay          = ay;
    out->az          = az;
    out->gx          = gx;
    out->gy          = gy;
    out->gz          = gz;
    out->pressure_pa = pressure_pa;
    out->temp_c100   = 2500;
    return ESP_OK;
}

bool sensors_get_latest(ember_sample_t *out)
{
    if (s_latest_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_latest_lock, portMAX_DELAY);
    bool have = s_have_latest;
    if (have) {
        *out = s_latest;
    }
    xSemaphoreGive(s_latest_lock);
    return have;
}

static void sensors_task(void *arg)
{
    (void)arg;

    TickType_t next     = xTaskGetTickCount();
    uint32_t   q_drops  = 0;
    uint32_t   last_warn_ms = 0;

    for (;;) {
        ember_sample_t s;
        sensors_read(&s);

        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
        s_latest      = s;
        s_have_latest = true;
        xSemaphoreGive(s_latest_lock);

        if (s_log_queue != NULL) {
            if (xQueueSend(s_log_queue, &s, 0) != pdTRUE) {
                q_drops++;
            }
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (last_warn_ms == 0) {
            last_warn_ms = now_ms;
        } else if (now_ms - last_warn_ms >= 5000 && q_drops) {
            ESP_LOGW(TAG, "in last %lu ms: %lu queue drops",
                     (unsigned long)(now_ms - last_warn_ms),
                     (unsigned long)q_drops);
            q_drops      = 0;
            last_warn_ms = now_ms;
        }

        vTaskDelayUntil(&next, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

esp_err_t sensors_start(QueueHandle_t log_queue)
{
    s_log_queue = log_queue;

    BaseType_t ok = xTaskCreate(sensors_task, "sensors",
                                TASK_STACK, NULL, TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
