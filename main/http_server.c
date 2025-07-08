#include "http_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define TAG "HTTP_SERVER"

static httpd_handle_t server = NULL;

static uint8_t *latest_frame = NULL;
static size_t latest_frame_len = 0;
static SemaphoreHandle_t frame_mutex;

static esp_err_t mjpeg_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "MJPEG stream connected");

    // HTTP headers for MJPEG streaming
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    while (1) {
        if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (latest_frame && latest_frame_len > 0) {
                char part_header[128];
                int hdr_len = snprintf(part_header, sizeof(part_header),
                    "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                    (int)latest_frame_len);

                if (httpd_resp_send_chunk(req, part_header, hdr_len) != ESP_OK) {
                    xSemaphoreGive(frame_mutex);
                    break;
                }
                if (httpd_resp_send_chunk(req, (const char *)latest_frame, latest_frame_len) != ESP_OK) {
                    xSemaphoreGive(frame_mutex);
                    break;
                }
            }
            xSemaphoreGive(frame_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // 20 fps approx
    }

    ESP_LOGI(TAG, "MJPEG stream disconnected");
    return ESP_FAIL; // Client disconnected
}

static esp_err_t index_handler(httpd_req_t *req) {
    const char *html = "<html><head><title>ESP32 MJPEG Stream</title></head>"
                   "<body><h2>ESP32 MJPEG Stream</h2>"
                   "<img src=\"/mjpeg\" style=\"width:100%; height:auto;\" />"
                   "</body></html>";	// 响应式写法
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

void http_server_start(void) {
    frame_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = index_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t mjpeg_uri = {
            .uri       = "/mjpeg",
            .method    = HTTP_GET,
            .handler   = mjpeg_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &mjpeg_uri);

        ESP_LOGI(TAG, "HTTP Server started");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP Server");
    }
}

void http_server_send_frame(uint8_t *jpeg, size_t len) {
    if (xSemaphoreTake(frame_mutex, 0) == pdTRUE) {
        if (latest_frame) {
            free(latest_frame);
            latest_frame = NULL;
        }
        latest_frame = malloc(len);
        if (latest_frame) {
            memcpy(latest_frame, jpeg, len);
            latest_frame_len = len;
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for JPEG frame");
            latest_frame_len = 0;
        }
        xSemaphoreGive(frame_mutex);
    }
}
