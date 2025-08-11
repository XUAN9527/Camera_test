#include "lcd_camera.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "board.h"
#include <string.h>

#define TAG "lcd_camera"

#define LCD_DISPLAY_EN	// 使能LCD显示

#define LCD_H_RES 320
#define LCD_V_RES 240
#define STREAM_FRAME_RATE 	15
#define DISPLAY_FRAME_RATE 	15
#define DISPLAY_SW_QUALITY	80 // 0~100

bool use_hardware_jpeg = false;	// true：传感器采集jpeg直接传输；false：传感器采集rgb565->jpeg转换传输。此前提下，type = 0/1。
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
    .jpeg_quality = 7,
    .fb_count = 3,
    .grab_mode = CAMERA_GRAB_LATEST,
    .fb_location = CAMERA_FB_IN_PSRAM,
};

esp_lcd_panel_handle_t lcd_camera_get_panel(void) {
    return panel_handle;
}

// jpg2rgb565原始函数显示花屏，需要.flags.swap_color_bytes = 1
static uint8_t work[3100];
bool myjpg2rgb565(const uint8_t *src, size_t src_len, uint8_t * out, esp_jpeg_image_scale_t scale)
{
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)src,
        .indata_size = src_len,
        .outbuf = out,
        .outbuf_size = UINT32_MAX, // @todo: this is very bold assumption, keeping this like this for now, not to break existing code
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
        .flags.swap_color_bytes = 1,
        .advanced.working_buffer = work,
        .advanced.working_buffer_size = sizeof(work),
    };

    esp_jpeg_image_output_t output_img = {};

    if(esp_jpeg_decode(&jpeg_cfg, &output_img) != ESP_OK){
        return false;
    }
    return true;
}

static volatile bool fb_used = false;
static void display_task(void *arg) {
    while (1) {
		if(fb_used)
		{
			vTaskDelay(pdMS_TO_TICKS(1));
			continue;
		}
		fb_used = true;

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "%s jpeg get failed!", use_hardware_jpeg ? "hardware" : "software");
			fb_used = false;
            vTaskDelay(pdMS_TO_TICKS(1000 / DISPLAY_FRAME_RATE));
            continue;
        }

        const int lines = 40;
        if (!use_hardware_jpeg) {
            // RGB565 直显，无需转换
            for (int i = 0; i < LCD_V_RES / lines; i++) {
                esp_lcd_panel_draw_bitmap(panel_handle,
                                          0, i * lines,
                                          LCD_H_RES, (i + 1) * lines,
                                          fb->buf + i * lines * LCD_H_RES * 2);
            }
        } else {
            // JPEG 解码为 RGB565 后显示
            size_t rgb565_len = LCD_H_RES * LCD_V_RES * 2;
            uint8_t *rgb565_buf = (uint8_t *)heap_caps_malloc(rgb565_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (rgb565_buf == NULL) {
                ESP_LOGE(TAG, "Failed to allocate RGB565 buffer");
                esp_camera_fb_return(fb);
                vTaskDelay(pdMS_TO_TICKS(1000 / DISPLAY_FRAME_RATE));
                continue;
            }

            if (myjpg2rgb565(fb->buf, fb->len, rgb565_buf, JPEG_IMAGE_SCALE_0)) {
                for (int i = 0; i < LCD_V_RES / lines; i++) {
                    esp_lcd_panel_draw_bitmap(panel_handle,
                                              0, i * lines,
                                              LCD_H_RES, (i + 1) * lines,
                                              rgb565_buf + i * lines * LCD_H_RES * 2);
                }
            } else {
                ESP_LOGE(TAG, "JPEG to RGB565 decode failed");
            }

            heap_caps_free(rgb565_buf);
        }

        esp_camera_fb_return(fb);
		fb_used = false;
        vTaskDelay(pdMS_TO_TICKS(1000 / DISPLAY_FRAME_RATE));
    }
}

#define JPEG_SOI0 0xFF
#define JPEG_SOI1 0xD8
#define JPEG_EOI0 0xFF
#define JPEG_EOI1 0xD9

// #include "mbedtls/base64.h"
// static void debug_print_jpeg_base64(const uint8_t *data, size_t len)
// {
//     if (!data || len == 0) return;

//     size_t encoded_len = 0;
//     mbedtls_base64_encode(NULL, 0, &encoded_len, data, len);
//     uint8_t *b64_buf = malloc(encoded_len + 1);
//     if (!b64_buf) {
//         ESP_LOGE(TAG, "base64 malloc failed");
//         return;
//     }

//     if (mbedtls_base64_encode(b64_buf, encoded_len, &encoded_len, data, len) == 0) {
//         b64_buf[encoded_len] = 0;
//         printf("\n===== JPEG FRAME START =====\n%s\n===== JPEG FRAME END =====\n", b64_buf);
//     } else {
//         ESP_LOGE(TAG, "base64 encode failed");
//     }

//     free(b64_buf);
// }

static void stream_task(void *arg) {
    while (1) {
		// ESP_LOGI(TAG, "send_jpeg ptr: %p, stream_flag ptr: %p", user_config.send_jpeg, user_config.stream_flag);
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
				ESP_LOGE(TAG, "%s jpeg get failed!", use_hardware_jpeg ? "hardware" : "software");
				fb_used = false;
				vTaskDelay(pdMS_TO_TICKS(1000 / DISPLAY_FRAME_RATE));
				continue;
			}

			if (use_hardware_jpeg) {
				if (fb->len > 100 && fb->buf[0] == JPEG_SOI0 && fb->buf[1] == JPEG_SOI1 &&
					fb->buf[fb->len - 2] == JPEG_EOI0 && fb->buf[fb->len - 1] == JPEG_EOI1) 
				{
					uint8_t type = fb->format == PIXFORMAT_JPEG ? 0 : 1;
					user_config.send_jpeg(fb->buf, fb->len, type);
					// 添加这一行，仅打印前几帧检查
					// static int printed = 0;
					// if (printed++ < 3) {
					// 	debug_print_jpeg_base64(fb->buf, fb->len);
					// }
				} else {
					ESP_LOGW(TAG, "Invalid HW JPEG frame, skipping...");
				}
			} else {
				uint8_t *jpeg_buf = NULL;
				size_t jpeg_len = 0;
				if (frame2jpg(fb, DISPLAY_SW_QUALITY, &jpeg_buf, &jpeg_len)) {
					uint8_t type = fb->format == PIXFORMAT_JPEG ? 0 : 1;
					user_config.send_jpeg(jpeg_buf, jpeg_len, type);
					free(jpeg_buf);
				} else {
					ESP_LOGW(TAG, "SW JPEG encode failed");
				}
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
	if (use_hardware_jpeg) {
		camera_config.pixel_format = PIXFORMAT_JPEG;
	} else {
		camera_config.pixel_format = PIXFORMAT_RGB565;
	}
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
