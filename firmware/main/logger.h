#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Mounts LittleFS, opens a new log file for this boot and
 * starts the writer task. *out_queue is the queue the sensor task
 * should post ember_sample_t records to */
esp_err_t logger_start(QueueHandle_t *out_queue);

typedef struct {
    uint32_t boot_count;
    uint32_t samples_written;
    uint32_t samples_dropped;
    uint64_t bytes_written;
    uint32_t fs_total_bytes;
    uint32_t fs_used_bytes;
    char     current_file[64];
} logger_stats_t;

void logger_get_stats(logger_stats_t *out);

#define LOGGER_MOUNT_POINT "/storage"
#define LOGGER_LOG_DIR     "/storage/logs"
