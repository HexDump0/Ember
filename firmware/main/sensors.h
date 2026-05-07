#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    uint32_t t_ms;
    int16_t  ax, ay, az;
    int16_t  gx, gy, gz;
    int32_t  pressure_pa;
    int16_t  temp_c100;
} ember_sample_t;

esp_err_t sensors_init(void);
esp_err_t sensors_read(ember_sample_t *out);

/* Starts the periodic sample task (100 Hz). Each successful sample is
 * copied into a shared latest_sample slot and posted to log_queue;
 * if the queue is full the sample is dropped so the sensor loop is
 * never back-pressured. log_queue may be NULL to skip forwarding. */
esp_err_t sensors_start(QueueHandle_t log_queue);

/* Snapshot of the most recent successful sample. Returns false until
 * the first sample has been taken. */
bool sensors_get_latest(ember_sample_t *out);
