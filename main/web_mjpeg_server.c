#include "web_mjpeg_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>

#define TAG "WEB_MJPEG"

static httpd_handle_t server = NULL;
static int last_fd = -1;

// HTML 网页，浏览器访问显示图像
static const char *html_page =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP32 MJPEG Stream</title></head>"
    "<body style='margin:0;padding:0;background:black;'>"
    "<img id='video' style='width:100vw;height:auto;' />"
    "<script>"
    "var socket = new WebSocket('ws://' + location.host + '/ws');"
    "socket.binaryType = 'arraybuffer';"
    "socket.onmessage = function(event) {"
    "  var blob = new Blob([event.data], { type: 'image/jpeg' });"
    "  var url = URL.createObjectURL(blob);"
    "  var img = document.getElementById('video');"
    "  if (img.src) URL.revokeObjectURL(img.src);"
    "  img.src = url;"
    "};"
    "socket.onclose = function(event) {"
    "  console.log('WebSocket closed');"
    "};"
    "socket.onerror = function(error) {"
    "  console.log('WebSocket error: ' + error);"
    "};"
    "</script></body></html>";

// HTTP GET "/" 返回网页
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// WebSocket 连接处理，直接记录客户端 fd，不调用 httpd_ws_recv_frame
static esp_err_t websocket_handler(httpd_req_t *req) {
    last_fd = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "WebSocket client connected, fd=%d", last_fd);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler
};

static const httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = websocket_handler,
    .is_websocket = true
};

void web_mjpeg_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 8;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &ws_uri);
        ESP_LOGI(TAG, "HTTP MJPEG WebSocket server started");
    }
}

bool web_mjpeg_server_is_client_connected(void) {
    return (last_fd >= 0);
}

// 推送 JPEG 图像到 WebSocket 客户端
void web_mjpeg_server_send_jpeg(const uint8_t *jpeg_buf, size_t jpeg_len) {
    if (!server || last_fd < 0 || !jpeg_buf || jpeg_len == 0) return;

    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)jpeg_buf,
        .len = jpeg_len,
    };

    esp_err_t err = httpd_ws_send_frame_async(server, last_fd, &ws_pkt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send JPEG over WebSocket: %d", err);
        last_fd = -1; // 断开连接
    } else {
        // ESP_LOGI(TAG, "JPEG sent, size=%d bytes", jpeg_len);
    }
}
