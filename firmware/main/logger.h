#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* rn a stub */
esp_err_t logger_start(QueueHandle_t *out_queue);
