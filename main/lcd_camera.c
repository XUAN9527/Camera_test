#include "lcd_camera.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "board.h"
#include "audio_mem.h"
#include <string.h>

#define TAG "lcd_camera"
#define LCD_H_RES 320
#define LCD_V_RES 240
#define FRAME_RATE 15

bool use_hardware_jpeg = false;
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
    .pixel_format = PIXFORMAT_JPEG, // PIXFORMAT_RGB565, PIXFORMAT_JPEG
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count = 4,
    .grab_mode = CAMERA_GRAB_LATEST,
    .fb_location = CAMERA_FB_IN_PSRAM,
};

esp_lcd_panel_handle_t lcd_camera_get_panel(void) {
    return panel_handle;
}

static void display_task(void *arg) {
    while (1) {
		camera_fb_t *fb = esp_camera_fb_get();
		if(fb){
            int lines = 40;
            for (int i = 0; i < LCD_V_RES / lines; i++) {
                esp_lcd_panel_draw_bitmap(panel_handle, 0, i * lines, LCD_H_RES, (i + 1) * lines, fb->buf + i * lines * LCD_H_RES * 2);
            }
            esp_camera_fb_return(fb);
        }else{
			ESP_LOGE(TAG, "fb get faild!");
		}
		vTaskDelay(pdMS_TO_TICKS(1000/FRAME_RATE));
    }
}

#define JPEG_SOI0 0xFF
#define JPEG_SOI1 0xD8
#define JPEG_EOI0 0xFF
#define JPEG_EOI1 0xD9

// 返回值：true表示完整，否则false
bool is_jpeg_complete(const uint8_t *buf, size_t len) {
    if (len < 4) return false;
    if (buf[0] != JPEG_SOI0 || buf[1] != JPEG_SOI1) return false;
    if (buf[len - 2] == JPEG_EOI0 && buf[len - 1] == JPEG_EOI1) return true;
    return false;
}

/**
 * 修复JPEG数据，确保末尾有 EOI 标志。
 * 
 * @param orig_buf  原始数据指针
 * @param orig_len  原始数据长度
 * @param out_len   输出修复后数据长度指针
 * @return          修复后数据指针，可能是orig_buf（不需要free），或者是新分配内存（需要调用free释放）
 */
uint8_t *jpeg_fix_eoi(const uint8_t *orig_buf, size_t orig_len, size_t *out_len) {
    *out_len = orig_len;

    if (is_jpeg_complete(orig_buf, orig_len)) {
        // 已完整，直接返回原指针
        return (uint8_t *)orig_buf;
    }

    // 缺少 EOI，分配新内存，复制数据并追加 EOI
    size_t new_len = orig_len + 2;
    uint8_t *new_buf = malloc(new_len);
    if (!new_buf) {
        // 内存分配失败，返回原始数据（风险自负）
        *out_len = orig_len;
        return (uint8_t *)orig_buf;
    }

    memcpy(new_buf, orig_buf, orig_len);
    new_buf[orig_len] = JPEG_EOI0;
    new_buf[orig_len + 1] = JPEG_EOI1;
    *out_len = new_len;

    return new_buf;
}

static void stream_task(void *arg) {
    while (1) {
		if (user_config.send_jpeg && user_config.stream_flag())
		{
			camera_fb_t *fb = esp_camera_fb_get();
			if (fb) {
				if (use_hardware_jpeg) {
					if (fb->len > 100 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
						fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9) {
						user_config.send_jpeg(fb->buf, fb->len);
					} else {
						ESP_LOGW(TAG, "Invalid HW JPEG frame, skipping...");
					}
				} else {
					uint8_t *jpeg_buf = NULL;
					size_t jpeg_len = 0;
					if (frame2jpg(fb, 60, &jpeg_buf, &jpeg_len)) {
						user_config.send_jpeg(jpeg_buf, jpeg_len);
						free(jpeg_buf);
					} else {
						ESP_LOGW(TAG, "SW JPEG encode failed");
					}
				}
				esp_camera_fb_return(fb);
			}
		}
		vTaskDelay(pdMS_TO_TICKS(1000/FRAME_RATE));
	}
}

esp_err_t lcd_camera_start(const lcd_camera_config_t *config) {
    user_config = *config;
	if (use_hardware_jpeg) {
		camera_config.pixel_format = PIXFORMAT_JPEG;
	} else {
		camera_config.pixel_format = PIXFORMAT_RGB565;
	}
    ESP_ERROR_CHECK(esp_camera_init(&camera_config));

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    panel_handle = audio_board_lcd_init(set, NULL);

    xTaskCreatePinnedToCore(display_task, "lcd_display", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(stream_task, "stream_task", 8192, NULL, 5, NULL, 1);

    return ESP_OK;
}
