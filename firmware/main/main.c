#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ember";

/* onboard LED on the ESP32-C3 Super Mini */
#define LED_GPIO        GPIO_NUM_8
#define BLINK_PERIOD_MS 500

static void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static inline void led_set(bool on)
{
    gpio_set_level(LED_GPIO, !on);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ember boot (target=%s)", CONFIG_IDF_TARGET);

    led_init();

    bool on = false;
    for (;;) {
        on = !on;
        led_set(on);
        vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
    }
}
