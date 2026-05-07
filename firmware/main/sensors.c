#include "sensors.h"

#include <string.h>

#include "bmp280.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2cdev.h"
#include "mpu6050.h"

#define I2C_PORT          I2C_NUM_0
#define SDA_GPIO          GPIO_NUM_8
#define SCL_GPIO          GPIO_NUM_9

#define SAMPLE_PERIOD_MS  10           /* 100 Hz */
#define TASK_STACK        4096
#define TASK_PRIO         6

static const char *TAG = "sensors";

static mpu6050_dev_t s_mpu;
static bmp280_t      s_bmp;

static SemaphoreHandle_t s_latest_lock;
static ember_sample_t    s_latest;
static bool              s_have_latest;

static QueueHandle_t s_log_queue;

esp_err_t sensors_init(void)
{
    ESP_ERROR_CHECK(i2cdev_init());

    memset(&s_mpu, 0, sizeof(s_mpu));
    ESP_ERROR_CHECK(mpu6050_init_desc(&s_mpu, MPU6050_I2C_ADDRESS_LOW,
                                      I2C_PORT, SDA_GPIO, SCL_GPIO));
    ESP_ERROR_CHECK(mpu6050_init(&s_mpu));
    ESP_LOGI(TAG, "MPU6050 ready @0x%02x", MPU6050_I2C_ADDRESS_LOW);

    bmp280_params_t params;
    ESP_ERROR_CHECK(bmp280_init_default_params(&params));

    memset(&s_bmp, 0, sizeof(s_bmp));
    ESP_ERROR_CHECK(bmp280_init_desc(&s_bmp, BMP280_I2C_ADDRESS_0,
                                     I2C_PORT, SDA_GPIO, SCL_GPIO));
    ESP_ERROR_CHECK(bmp280_init(&s_bmp, &params));
    ESP_LOGI(TAG, "BMP280 ready @0x%02x (chip 0x%02x)",
             BMP280_I2C_ADDRESS_0, s_bmp.id);

    s_latest_lock = xSemaphoreCreateMutex();
    if (s_latest_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t sensors_read(ember_sample_t *out)
{
    mpu6050_raw_acceleration_t accel;
    mpu6050_raw_rotation_t     gyro;

    esp_err_t err = mpu6050_get_raw_acceleration(&s_mpu, &accel);
    if (err != ESP_OK) {
        return err;
    }
    err = mpu6050_get_raw_rotation(&s_mpu, &gyro);
    if (err != ESP_OK) {
        return err;
    }

    int32_t  temp_c100;
    uint32_t pressure_q24_8;
    err = bmp280_read_fixed(&s_bmp, &temp_c100, &pressure_q24_8, NULL);
    if (err != ESP_OK) {
        return err;
    }

    out->t_ms        = (uint32_t)(esp_timer_get_time() / 1000);
    out->ax          = accel.x;
    out->ay          = accel.y;
    out->az          = accel.z;
    out->gx          = gyro.x;
    out->gy          = gyro.y;
    out->gz          = gyro.z;
    out->pressure_pa = (int32_t)(pressure_q24_8 >> 8);
    out->temp_c100   = (int16_t)temp_c100;

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

    TickType_t next = xTaskGetTickCount();
    uint32_t   read_errs = 0;
    uint32_t   q_drops   = 0;
    uint32_t   last_warn_ms = 0;

    for (;;) {
        ember_sample_t s;
        esp_err_t err = sensors_read(&s);
        if (err == ESP_OK) {
            xSemaphoreTake(s_latest_lock, portMAX_DELAY);
            s_latest      = s;
            s_have_latest = true;
            xSemaphoreGive(s_latest_lock);

            if (s_log_queue != NULL) {
                if (xQueueSend(s_log_queue, &s, 0) != pdTRUE) {
                    q_drops++;
                }
            }
        } else {
            read_errs++;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_warn_ms >= 5000 && (read_errs || q_drops)) {
            ESP_LOGW(TAG, "in last %lu ms: %lu read errs, %lu queue drops",
                     (unsigned long)(now_ms - last_warn_ms),
                     (unsigned long)read_errs,
                     (unsigned long)q_drops);
            read_errs = 0;
            q_drops   = 0;
            last_warn_ms = now_ms;
        } else if (last_warn_ms == 0) {
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
