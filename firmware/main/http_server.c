#include "http_server.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "logger.h"
#include "sensors.h"

#define MAX_WS_CLIENTS    4
#define WS_PUMP_PERIOD_MS 10              /* 100 Hz */
#define WS_TASK_STACK     4096
#define WS_TASK_PRIO      3

static const char *TAG = "httpd";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t    s_server;
static SemaphoreHandle_t s_ws_lock;
static int               s_ws_clients[MAX_WS_CLIENTS];

static void ws_clients_init(void)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_ws_clients[i] = -1;
    }
}

static bool ws_clients_add(int fd)
{
    bool added = false;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i] == fd) { added = true; break; }
        if (s_ws_clients[i] == -1) { s_ws_clients[i] = fd; added = true; break; }
    }
    xSemaphoreGive(s_ws_lock);
    return added;
}

static void ws_clients_remove(int fd)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i] == fd) { s_ws_clients[i] = -1; break; }
    }
    xSemaphoreGive(s_ws_lock);
}

static int ws_clients_count(void)
{
    int n = 0;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i] >= 0) n++;
    }
    xSemaphoreGive(s_ws_lock);
    return n;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    logger_stats_t st;
    logger_get_stats(&st);

    ember_sample_t s;
    bool have_sample = sensors_get_latest(&s);

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{"
          "\"uptime_ms\":%" PRIu64 ","
          "\"boot_count\":%" PRIu32 ","
          "\"current_file\":\"%s\","
          "\"samples_written\":%" PRIu32 ","
          "\"samples_dropped\":%" PRIu32 ","
          "\"bytes_written\":%" PRIu64 ","
          "\"fs_used\":%" PRIu32 ","
          "\"fs_total\":%" PRIu32,
        (uint64_t)(esp_timer_get_time() / 1000),
        st.boot_count, st.current_file,
        st.samples_written, st.samples_dropped, st.bytes_written,
        st.fs_used_bytes, st.fs_total_bytes);

    if (have_sample && n > 0 && n < (int)sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"sample\":{"
              "\"t_ms\":%" PRIu32 ","
              "\"ax\":%d,\"ay\":%d,\"az\":%d,"
              "\"gx\":%d,\"gy\":%d,\"gz\":%d,"
              "\"pressure_pa\":%" PRId32 ","
              "\"temp_c100\":%d"
            "}",
            s.t_ms, s.ax, s.ay, s.az, s.gx, s.gy, s.gz,
            s.pressure_pa, s.temp_c100);
    }
    if (n > 0 && n < (int)sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - n, "}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* Reject names that could escape LOGGER_LOG_DIR. */
static bool log_name_ok(const char *n)
{
    if (!n || !*n || strlen(n) > 48) return false;
    for (const char *p = n; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    if (strstr(n, "..") != NULL) return false;
    return true;
}

static esp_err_t logs_list_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    DIR *d = opendir(LOGGER_LOG_DIR);
    if (!d) {
        return httpd_resp_sendstr(req, "[]");
    }

    logger_stats_t st;
    logger_get_stats(&st);
    const char *cur = strrchr(st.current_file, '/');
    cur = cur ? cur + 1 : st.current_file;

    httpd_resp_send_chunk(req, "[", 1);

    bool first = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;

        char path[280];
        snprintf(path, sizeof(path), "%s/%s", LOGGER_LOG_DIR, e->d_name);
        struct stat sb;
        if (stat(path, &sb) != 0) continue;

        char buf[160];
        int n = snprintf(buf, sizeof(buf),
            "%s{\"name\":\"%s\",\"size\":%lu,\"active\":%s}",
            first ? "" : ",",
            e->d_name,
            (unsigned long)sb.st_size,
            (strcmp(e->d_name, cur) == 0) ? "true" : "false");
        if (n > 0) {
            httpd_resp_send_chunk(req, buf, n);
            first = false;
        }
    }
    closedir(d);

    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    const char prefix[] = "/api/logs/";
    if (strncmp(req->uri, prefix, sizeof(prefix) - 1) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const char *name = req->uri + sizeof(prefix) - 1;
    if (!log_name_ok(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }

    char path[280];
    snprintf(path, sizeof(path), "%s/%s", LOGGER_LOG_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such log");
    }

    char disp[160];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(fp);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t logs_delete_handler(httpd_req_t *req)
{
    const char prefix[] = "/api/logs/";
    if (strncmp(req->uri, prefix, sizeof(prefix) - 1) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const char *name = req->uri + sizeof(prefix) - 1;
    if (!log_name_ok(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }

    logger_stats_t st;
    logger_get_stats(&st);
    const char *cur = strrchr(st.current_file, '/');
    cur = cur ? cur + 1 : st.current_file;
    if (cur && strcmp(name, cur) == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "log is active");
    }

    char path[280];
    snprintf(path, sizeof(path), "%s/%s", LOGGER_LOG_DIR, name);
    if (unlink(path) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such log");
    }

    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        if (!ws_clients_add(fd)) {
            ESP_LOGW(TAG, "ws client fd=%d rejected (full)", fd);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "ws client connected fd=%d (%d total)",
                 fd, ws_clients_count());
        return ESP_OK;
    }

    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (f.len > 0 && f.len < 256) {
        uint8_t scratch[256];
        f.payload = scratch;
        httpd_ws_recv_frame(req, &f, f.len);
    }
    return ESP_OK;
}

static void ws_disconnect_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ws_clients_remove(sockfd);
    close(sockfd);
}

static void ws_pump_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    char buf[256];

    for (;;) {
        if (ws_clients_count() == 0 || !s_server) {
            vTaskDelayUntil(&next, pdMS_TO_TICKS(100));
            continue;
        }

        ember_sample_t s;
        if (sensors_get_latest(&s)) {
            int n = snprintf(buf, sizeof(buf),
                "{\"t_ms\":%" PRIu32 ","
                "\"ax\":%d,\"ay\":%d,\"az\":%d,"
                "\"gx\":%d,\"gy\":%d,\"gz\":%d,"
                "\"pressure_pa\":%" PRId32 ","
                "\"temp_c100\":%d}",
                s.t_ms, s.ax, s.ay, s.az, s.gx, s.gy, s.gz,
                s.pressure_pa, s.temp_c100);

            if (n > 0 && n < (int)sizeof(buf)) {
                int fds[MAX_WS_CLIENTS];
                int count = 0;
                xSemaphoreTake(s_ws_lock, portMAX_DELAY);
                for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                    if (s_ws_clients[i] >= 0) fds[count++] = s_ws_clients[i];
                }
                xSemaphoreGive(s_ws_lock);

                for (int i = 0; i < count; i++) {
                    httpd_ws_frame_t frame = {
                        .final   = true,
                        .type    = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t *)buf,
                        .len     = (size_t)n,
                    };
                    esp_err_t err = httpd_ws_send_frame_async(s_server,
                                                              fds[i], &frame);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "ws fd=%d send err=%s; dropping client",
                                 fds[i], esp_err_to_name(err));
                        ws_clients_remove(fds[i]);
                    }
                }
            }
        }

        vTaskDelayUntil(&next, pdMS_TO_TICKS(WS_PUMP_PERIOD_MS));
    }
}

esp_err_t http_server_start(void)
{
    s_ws_lock = xSemaphoreCreateMutex();
    if (s_ws_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ws_clients_init();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable     = true;
    cfg.stack_size           = 4096;
    cfg.max_uri_handlers     = 8;
    cfg.uri_match_fn         = httpd_uri_match_wildcard;
    cfg.close_fn             = ws_disconnect_cb;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    static const httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_handler,
    };
    static const httpd_uri_t uri_logs = {
        .uri = "/api/logs", .method = HTTP_GET, .handler = logs_list_handler,
    };
    static const httpd_uri_t uri_log_get = {
        .uri = "/api/logs/?*", .method = HTTP_GET, .handler = logs_get_handler,
    };
    static const httpd_uri_t uri_log_del = {
        .uri = "/api/logs/?*", .method = HTTP_DELETE, .handler = logs_delete_handler,
    };
    static const httpd_uri_t uri_ws = {
        .uri              = "/ws",
        .method           = HTTP_GET,
        .handler          = ws_handler,
        .is_websocket     = true,
        .handle_ws_control_frames = false,
    };
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_logs);
    httpd_register_uri_handler(s_server, &uri_log_get);
    httpd_register_uri_handler(s_server, &uri_log_del);
    httpd_register_uri_handler(s_server, &uri_ws);

    BaseType_t ok = xTaskCreate(ws_pump_task, "ws_pump",
                                WS_TASK_STACK, NULL, WS_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ws_pump_task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "http server up on :%d", cfg.server_port);
    return ESP_OK;
}
