#include "lcd_camera.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "board.h"
#include "audio_mem.h"

#define TAG "lcd_camera"
#define LCD_H_RES 320
#define LCD_V_RES 240
#define FRAME_QUEUE_LEN 2

static esp_lcd_panel_handle_t panel_handle = NULL;
// static QueueHandle_t frame_queue;
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
    .pixel_format = PIXFORMAT_RGB565,
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
        // camera_fb_t *fb = NULL;
        // if (xQueueReceive(frame_queue, &fb, portMAX_DELAY) == pdTRUE && fb) {
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
		vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void stream_task(void *arg) {
    while (1) {
		if ((user_config.send_jpeg) && (user_config.stream_flag) && (user_config.stream_flag()))
		{
			camera_fb_t *fb = esp_camera_fb_get();
			if(fb){
				uint8_t *jpeg_buf = NULL;
				size_t jpeg_len = 0;
				if (frame2jpg(fb, 60, &jpeg_buf, &jpeg_len)) {
					user_config.send_jpeg(jpeg_buf, jpeg_len);
					free(jpeg_buf);
				}
				esp_camera_fb_return(fb);
			}else{
				ESP_LOGE(TAG, "fb get faild!");
			}
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

esp_err_t lcd_camera_start(const lcd_camera_config_t *config) {
    user_config = *config;
    // frame_queue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(camera_fb_t *));
    ESP_ERROR_CHECK(esp_camera_init(&camera_config));

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    panel_handle = audio_board_lcd_init(set, NULL);

    xTaskCreatePinnedToCore(display_task, "lcd_display", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(stream_task, "stream_task", 8192, NULL, 4, NULL, 1);

    return ESP_OK;
}
