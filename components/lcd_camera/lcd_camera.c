#include "lcd_camera.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "board.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define TAG "lcd_camera"

#define LCD_DISPLAY_EN	// 使能LCD显示

#define LCD_H_RES 320
#define LCD_V_RES 240
#define STREAM_FRAME_RATE 	15
#define DISPLAY_FRAME_RATE 	15
#define DISPLAY_SW_QUALITY	80 // 0~100

// SC101 不支持硬件 JPEG，强制软件路径
static esp_lcd_panel_handle_t panel_handle = NULL;
static lcd_camera_config_t user_config;

static camera_config_t camera_config = {
    // 使用你的实际引脚定义
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
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_YUV422, // or PIXFORMAT_RGB565
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 7,
    .fb_count = 3,
    .grab_mode = CAMERA_GRAB_LATEST,
    .fb_location = CAMERA_FB_IN_PSRAM,
};

////////////////////////////////////////////////////////////////////////////////
// YUV422 (YUYV: Y0 U Y1 V) -> RGB565 conversion
// Notes:
//  - Input: src points to YUV422 buffer (fb->buf), length should be width*height*2
//  - Output: dst is width*height*2 bytes (RGB565 little-endian by default).
//  - SWAP_BYTE_OUTPUT: if your LCD expects swapped byte order, set to 1.
//    You observed earlier JPEG decode needed swap_color_bytes=1; 若颜色/字节顺序不对，改为 1。
////////////////////////////////////////////////////////////////////////////////
#define SWAP_RGB565_BYTES 0

static inline int clamp(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void yuv422_to_rgb565(const uint8_t *src, uint8_t *dst, int width, int height)
{
    // src length = width * height * 2
    // dst length = width * height * 2 (uint16 per pixel)
    const uint8_t *s = src;
    uint8_t *d = dst;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x += 2) {
            // Y0 U Y1 V
            int Y0 = s[0];
            int U  = s[1];
            int Y1 = s[2];
            int V  = s[3];
            s += 4;

            int C0 = Y0 - 16;
            int C1 = Y1 - 16;
            int D = U - 128;
            int E = V - 128;

            // using integer arithmetic similar to ITU-R BT.601
            int R0 = (298 * C0 + 409 * E + 128) >> 8;
            int G0 = (298 * C0 - 100 * D - 208 * E + 128) >> 8;
            int B0 = (298 * C0 + 516 * D + 128) >> 8;

            int R1 = (298 * C1 + 409 * E + 128) >> 8;
            int G1 = (298 * C1 - 100 * D - 208 * E + 128) >> 8;
            int B1 = (298 * C1 + 516 * D + 128) >> 8;

            R0 = clamp(R0); G0 = clamp(G0); B0 = clamp(B0);
            R1 = clamp(R1); G1 = clamp(G1); B1 = clamp(B1);

            uint16_t p0 = ((R0 >> 3) << 11) | ((G0 >> 2) << 5) | (B0 >> 3);
            uint16_t p1 = ((R1 >> 3) << 11) | ((G1 >> 2) << 5) | (B1 >> 3);

            if (SWAP_RGB565_BYTES) {
                // write little endian swapped bytes if LCD expects that
                d[0] = (uint8_t)(p0 & 0xFF);
                d[1] = (uint8_t)(p0 >> 8);
                d[2] = (uint8_t)(p1 & 0xFF);
                d[3] = (uint8_t)(p1 >> 8);
            } else {
                // big endian order
                d[0] = (uint8_t)(p0 >> 8);
                d[1] = (uint8_t)(p0 & 0xFF);
                d[2] = (uint8_t)(p1 >> 8);
                d[3] = (uint8_t)(p1 & 0xFF);
            }
            d += 4;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

static volatile bool fb_used = false;
static void display_task(void *arg) {
    const int lines = 40; // 分块绘制行数，可调
    while (1) {
        if(fb_used) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        fb_used = true;

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "FB get failed!");
            fb_used = false;
            vTaskDelay(pdMS_TO_TICKS(1000 / DISPLAY_FRAME_RATE));
            continue;
        }

        if (fb->format == PIXFORMAT_YUV422) {
            // YUV422 → RGB565
            size_t rgb565_len = fb->width * fb->height * 2;
            uint8_t *rgb565_buf = (uint8_t *)heap_caps_malloc(rgb565_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!rgb565_buf) {
                ESP_LOGE(TAG, "Failed to allocate RGB565 buffer");
                esp_camera_fb_return(fb);
                fb_used = false;
                vTaskDelay(pdMS_TO_TICKS(1000 / DISPLAY_FRAME_RATE));
                continue;
            }

            yuv422_to_rgb565(fb->buf, rgb565_buf, fb->width, fb->height);

            for (int i = 0; i < fb->height / lines; i++) {
                esp_lcd_panel_draw_bitmap(panel_handle,
                                          0, i * lines,
                                          fb->width, (i + 1) * lines,
                                          rgb565_buf + i * lines * fb->width * 2);
            }

            heap_caps_free(rgb565_buf);
        } 
        else if (fb->format == PIXFORMAT_RGB565) {
            // RGB565 直出
            for (int i = 0; i < fb->height / lines; i++) {
                esp_lcd_panel_draw_bitmap(panel_handle,
                                          0, i * lines,
                                          fb->width, (i + 1) * lines,
                                          fb->buf + i * lines * fb->width * 2);
            }
        }
        else {
            ESP_LOGE(TAG, "Unsupported FB format: %d", fb->format);
        }

        esp_camera_fb_return(fb);
        fb_used = false;
        vTaskDelay(pdMS_TO_TICKS(1000 / DISPLAY_FRAME_RATE));
    }
}

static void stream_task(void *arg) {
    while (1) {
        if (user_config.send_jpeg && user_config.stream_flag && user_config.stream_flag())
        {
            if(fb_used)
            {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            fb_used = true;
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                fb_used = false;
                vTaskDelay(pdMS_TO_TICKS(1000 / STREAM_FRAME_RATE));
                continue;
            }

            uint8_t *jpeg_buf = NULL;
            size_t jpeg_len = 0;

            // 直接使用 YUV422 转 JPEG
            if (fb->format == PIXFORMAT_YUV422) {
                if (frame2jpg(fb, DISPLAY_SW_QUALITY, &jpeg_buf, &jpeg_len)) {
                    user_config.send_jpeg(jpeg_buf, jpeg_len, 1); // type=1 表示软件 JPEG
                    free(jpeg_buf);
                } else {
                    ESP_LOGW(TAG, "SW JPEG encode failed");
                }
            }
            // 如果未来有 RGB565 FB
            else if (fb->format == PIXFORMAT_RGB565) {
                if (frame2jpg(fb, DISPLAY_SW_QUALITY, &jpeg_buf, &jpeg_len)) {
                    user_config.send_jpeg(jpeg_buf, jpeg_len, 1);
                    free(jpeg_buf);
                } else {
                    ESP_LOGW(TAG, "SW JPEG encode failed");
                }
            }
            else {
                ESP_LOGE(TAG, "Unsupported FB format: %d", fb->format);
            }

            esp_camera_fb_return(fb);
            fb_used = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000/STREAM_FRAME_RATE));
    }
}

esp_err_t lcd_camera_start(const lcd_camera_config_t *config) {
	if (config == NULL || config->send_jpeg == NULL || config->stream_flag == NULL) {
        ESP_LOGE(TAG, "Invalid lcd_camera_config! send_jpeg or stream_flag is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    user_config = *config;
    ESP_ERROR_CHECK(esp_camera_init(&camera_config));
	vTaskDelay(pdMS_TO_TICKS(300));
#ifdef LCD_DISPLAY_EN
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    panel_handle = audio_board_lcd_init(set, NULL);

    xTaskCreatePinnedToCore(display_task, "lcd_display", 8192, NULL, 4, NULL, 0);
#endif
    xTaskCreatePinnedToCore(stream_task, "stream_task", 8192, NULL, 5, NULL, 1);

    return ESP_OK;
}
