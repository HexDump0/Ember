#include "logger.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sensors.h"

#define QUEUE_DEPTH       128
#define TASK_STACK        4096
#define TASK_PRIO         4
#define REPORT_PERIOD_MS  1000
#define BATCH_SAMPLES     42                 /* ~1 KB at 24 B/sample */
#define FLUSH_PERIOD_MS   1000
#define ROTATE_THRESHOLD  80

#define LOG_MAGIC         0x52424d45u        /* 'EMBR' little-endian */
#define LOG_VERSION       1
#define NVS_NAMESPACE     "ember"
#define NVS_KEY_BOOTCOUNT "bootcount"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t sample_size;
    uint32_t boot_count;
    uint32_t header_t_ms;
    uint8_t  reserved[16];
} ember_log_header_t;

_Static_assert(sizeof(ember_log_header_t) == 32, "log header must be 32 bytes");

static const char *TAG = "logger";

static QueueHandle_t      s_queue;
static FILE              *s_fp;
static SemaphoreHandle_t  s_stats_lock;
static logger_stats_t     s_stats;
static ember_sample_t     s_batch[BATCH_SAMPLES];
static size_t             s_batch_count;
static uint32_t           s_last_flush_ms;
static uint32_t           s_last_report_ms;

static esp_err_t mount_fs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path        = LOGGER_MOUNT_POINT,
        .partition_label  = "storage",
        .format_if_mount_failed = true,
        .dont_mount       = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted: %u/%u bytes used",
                 (unsigned)used, (unsigned)total);
        s_stats.fs_total_bytes = (uint32_t)total;
        s_stats.fs_used_bytes  = (uint32_t)used;
    }

    struct stat st;
    if (stat(LOGGER_LOG_DIR, &st) != 0) {
        if (mkdir(LOGGER_LOG_DIR, 0775) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed", LOGGER_LOG_DIR);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static uint32_t bump_boot_count(void)
{
    nvs_handle_t h;
    uint32_t bc = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_BOOTCOUNT, &bc);
        bc++;
        nvs_set_u32(h, NVS_KEY_BOOTCOUNT, bc);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs unavailable; boot count not persisted");
        bc = (uint32_t)(esp_timer_get_time() / 1000);
    }
    return bc;
}

static bool find_oldest_log(const char *current, char *out, size_t out_sz)
{
    DIR *d = opendir(LOGGER_LOG_DIR);
    if (!d) {
        return false;
    }
    char best[64] = {0};
    bool have = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;
        if (current && strcmp(e->d_name, current) == 0) continue;
        if (!have || strcmp(e->d_name, best) < 0) {
            strncpy(best, e->d_name, sizeof(best) - 1);
            best[sizeof(best) - 1] = '\0';
            have = true;
        }
    }
    closedir(d);
    if (have && out) {
        strncpy(out, best, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
    return have;
}

static void rotate_if_needed(void)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) != ESP_OK || total == 0) {
        return;
    }
    s_stats.fs_total_bytes = (uint32_t)total;
    s_stats.fs_used_bytes  = (uint32_t)used;

    while (used * 100 / total >= ROTATE_THRESHOLD) {
        char victim[64];
        const char *cur = strrchr(s_stats.current_file, '/');
        cur = cur ? cur + 1 : s_stats.current_file;
        if (!find_oldest_log(cur, victim, sizeof(victim))) {
            break;
        }
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", LOGGER_LOG_DIR, victim);
        if (unlink(path) != 0) {
            ESP_LOGW(TAG, "unlink %s failed", path);
            break;
        }
        ESP_LOGW(TAG, "rotated out %s (fs %u/%u)",
                 path, (unsigned)used, (unsigned)total);
        if (esp_littlefs_info("storage", &total, &used) != ESP_OK) break;
    }
    s_stats.fs_total_bytes = (uint32_t)total;
    s_stats.fs_used_bytes  = (uint32_t)used;
}

static esp_err_t open_log_file(uint32_t boot_count)
{
    snprintf(s_stats.current_file, sizeof(s_stats.current_file),
             "%s/flight-%05lu.bin", LOGGER_LOG_DIR,
             (unsigned long)boot_count);

    s_fp = fopen(s_stats.current_file, "wb");
    if (!s_fp) {
        ESP_LOGE(TAG, "open %s failed", s_stats.current_file);
        return ESP_FAIL;
    }

    ember_log_header_t hdr = {
        .magic       = LOG_MAGIC,
        .version     = LOG_VERSION,
        .sample_size = (uint16_t)sizeof(ember_sample_t),
        .boot_count  = boot_count,
        .header_t_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    if (fwrite(&hdr, sizeof(hdr), 1, s_fp) != 1) {
        ESP_LOGE(TAG, "header write failed");
        fclose(s_fp);
        s_fp = NULL;
        return ESP_FAIL;
    }
    fflush(s_fp);
    s_stats.bytes_written = sizeof(hdr);
    ESP_LOGI(TAG, "logging to %s (boot %lu)",
             s_stats.current_file, (unsigned long)boot_count);
    return ESP_OK;
}

static void flush_batch(void)
{
    if (!s_fp || s_batch_count == 0) {
        return;
    }
    size_t n = fwrite(s_batch, sizeof(ember_sample_t), s_batch_count, s_fp);
    fflush(s_fp);

    xSemaphoreTake(s_stats_lock, portMAX_DELAY);
    s_stats.samples_written += (uint32_t)n;
    s_stats.bytes_written   += (uint64_t)n * sizeof(ember_sample_t);
    xSemaphoreGive(s_stats_lock);

    if (n != s_batch_count) {
        ESP_LOGE(TAG, "short write: %u/%u", (unsigned)n, (unsigned)s_batch_count);
    }
    s_batch_count = 0;
}

static void logger_task(void *arg)
{
    (void)arg;
    s_last_flush_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_report_ms = s_last_flush_ms;
    uint32_t period_count = 0;

    for (;;) {
        ember_sample_t s;
        if (xQueueReceive(s_queue, &s, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_batch[s_batch_count++] = s;
            period_count++;
            if (s_batch_count >= BATCH_SAMPLES) {
                flush_batch();
            }
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - s_last_flush_ms >= FLUSH_PERIOD_MS) {
            flush_batch();
            rotate_if_needed();
            s_last_flush_ms = now;
        }

        uint32_t dt = now - s_last_report_ms;
        if (dt >= REPORT_PERIOD_MS) {
            UBaseType_t depth = uxQueueMessagesWaiting(s_queue);
            ESP_LOGI(TAG,
                     "wrote %lu samples (%lu Hz) total=%lu fs=%u/%u q=%u",
                     (unsigned long)period_count,
                     (unsigned long)((period_count * 1000UL) / dt),
                     (unsigned long)s_stats.samples_written,
                     (unsigned)s_stats.fs_used_bytes,
                     (unsigned)s_stats.fs_total_bytes,
                     (unsigned)depth);
            period_count     = 0;
            s_last_report_ms = now;
        }
    }
}

void logger_get_stats(logger_stats_t *out)
{
    if (!out) return;
    if (s_stats_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_stats_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_stats_lock);
}

esp_err_t logger_start(QueueHandle_t *out_queue)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init: %s", esp_err_to_name(err));
    }

    s_stats_lock = xSemaphoreCreateMutex();
    if (s_stats_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (mount_fs() != ESP_OK) {
        return ESP_FAIL;
    }

    s_stats.boot_count = bump_boot_count();
    rotate_if_needed();

    if (open_log_file(s_stats.boot_count) != ESP_OK) {
        return ESP_FAIL;
    }

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
