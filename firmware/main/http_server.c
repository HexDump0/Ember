#include "http_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "logger.h"
#include "sensors.h"

static const char *TAG = "httpd";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

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

esp_err_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 4096;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
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
    httpd_register_uri_handler(srv, &uri_root);
    httpd_register_uri_handler(srv, &uri_status);

    ESP_LOGI(TAG, "http server up on :%d", cfg.server_port);
    return ESP_OK;
}
