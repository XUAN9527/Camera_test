/* LCD Camera with AP UDP Streaming - RGB565 Only */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "board.h"
#include "audio_mem.h"
#include "esp_camera.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/queue.h"
#include "esp_jpeg_common.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include <inttypes.h>

static const char *TAG = "LCD_Camera";

// 显示分辨率
#define EXAMPLE_LCD_H_RES   320
#define EXAMPLE_LCD_V_RES   240
#define EXAMPLE_FRAME_SIZE  8   // 降低帧率

// 摄像头配置
static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    .xclk_freq_hz = 40000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count = 2,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

// HTTP服务器句柄
static httpd_handle_t stream_httpd = NULL;

// MJPEG流处理函数
static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[128];

    res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    if (res != ESP_OK) {
        return res;
    }

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        uint8_t *jpeg_buf_ptr = NULL;
        size_t jpeg_size = 0;

        if (!frame2jpg(fb, 60, &jpeg_buf_ptr, &jpeg_size)) {
            ESP_LOGE(TAG, "JPEG compression failed");
            esp_camera_fb_return(fb);
            continue;
        }

        int header_len = snprintf(part_buf, sizeof(part_buf),
                                  "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                  (unsigned int)jpeg_size);

        if (header_len < 0 || header_len >= sizeof(part_buf)) {
            ESP_LOGE(TAG, "Header formatting failed");
            esp_camera_fb_return(fb);
            free(jpeg_buf_ptr);
            continue;
        }

        res = httpd_resp_send_chunk(req, part_buf, header_len);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Send header failed: %s", esp_err_to_name(res));
            esp_camera_fb_return(fb);
            free(jpeg_buf_ptr);
            break;
        }

        res = httpd_resp_send_chunk(req, (const char *)jpeg_buf_ptr, jpeg_size);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Send image failed: %s", esp_err_to_name(res));
            esp_camera_fb_return(fb);
            free(jpeg_buf_ptr);
            break;
        }

        res = httpd_resp_send_chunk(req, "\r\n", 2);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Send frame end failed: %s", esp_err_to_name(res));
            esp_camera_fb_return(fb);
            free(jpeg_buf_ptr);
            break;
        }

        esp_camera_fb_return(fb);
        free(jpeg_buf_ptr);

        vTaskDelay(pdMS_TO_TICKS(1000 / EXAMPLE_FRAME_SIZE));
    }

    if (fb) {
        esp_camera_fb_return(fb);
    }

    return res;
}

// 首页 HTML
static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html = "<html><head><title>ESP32 Camera</title></head>"
                       "<body><h1>ESP32 Camera</h1><img src=\"/stream\"></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

// 启动 HTTPD
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32760;
    config.max_open_sockets = 2;

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &index_uri);
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

static esp_err_t init_camera()
{
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Largest free block: %u bytes", (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return ESP_OK;
}

static esp_err_t example_lcd_rgb_draw(esp_lcd_panel_handle_t panel_handle, uint8_t *image)
{
    uint32_t lines_num = 40;
    for (int i = 0; i < EXAMPLE_LCD_V_RES / lines_num; ++i) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, i * lines_num, EXAMPLE_LCD_H_RES, lines_num + i * lines_num, image + EXAMPLE_LCD_H_RES * i * lines_num * 2);
    }
    return ESP_OK;
}

void my_lcd_camera_task(void *pvParameters)
{
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    if (ESP_OK != init_camera()) {
        vTaskDelete(NULL);
        return;
    }

    esp_lcd_panel_handle_t panel_handle = audio_board_lcd_init(set, NULL);
    if (!panel_handle) {
        ESP_LOGE(TAG, "LCD initialization failed");
        vTaskDelete(NULL);
        return;
    }

    start_webserver();

    while (1) {
        camera_fb_t *pic = esp_camera_fb_get();
        if (!pic) {
            vTaskDelay(1);
            continue;
        }

        example_lcd_rgb_draw(panel_handle, pic->buf);
        esp_camera_fb_return(pic);

        static int counter = 0;
        if (++counter % 10 == 0) {
            AUDIO_MEM_SHOW(TAG);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void my_lcd_camera_init(void)
{
    xTaskCreatePinnedToCore(
        my_lcd_camera_task,
        "cam_task",
        8192,    // ⬅️ 提高任务栈
        NULL,
        4,
        NULL,
        1
    );
}
