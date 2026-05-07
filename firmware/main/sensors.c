#include "sensors.h"

#include <string.h>

#include "bmp280.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2cdev.h"
#include "mpu6050.h"

#define I2C_PORT  I2C_NUM_0
#define SDA_GPIO  GPIO_NUM_8
#define SCL_GPIO  GPIO_NUM_9

static const char *TAG = "sensors";

static mpu6050_dev_t s_mpu;
static bmp280_t      s_bmp;

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
