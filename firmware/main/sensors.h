#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t t_ms;
    int16_t  ax, ay, az;
    int16_t  gx, gy, gz;
    int32_t  pressure_pa;
    int16_t  temp_c100;
} ember_sample_t;

esp_err_t sensors_init(void);
esp_err_t sensors_read(ember_sample_t *out);
