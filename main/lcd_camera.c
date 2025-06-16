/* LCD Camera with AP UDP Streaming - RGB565 Only */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "board.h"
#include "audio_mem.h"
#include "esp_camera.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/queue.h"
#include "udp_transmitter.h"
#include "esp_jpeg_common.h"

static const char *TAG = "LCD_Camera";

// 显示分辨率
#define EXAMPLE_LCD_H_RES   320
#define EXAMPLE_LCD_V_RES   240

// 图像队列 - 用于UDP传输
static QueueHandle_t image_queue = NULL;

// 摄像头配置 - 仅使用RGB565
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
    .pixel_format = PIXFORMAT_RGB565,  // 仅使用RGB565格式
    .frame_size = FRAMESIZE_QVGA,      // 320x240
    .jpeg_quality = 12,                 // 不相关
    .fb_count = 2,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,   // 总是获取最新帧
};

static esp_err_t init_camera()
{
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Camera initialized in RGB565 format");
    return ESP_OK;
}

/* example: RGB565 -> LCD */
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
    // 创建图像队列 (用于UDP传输)
    image_queue = xQueueCreate(3, sizeof(camera_fb_t *));
    if (!image_queue) {
        ESP_LOGE(TAG, "Failed to create image queue");
        vTaskDelete(NULL);
        return;
    }
    
    // 启动UDP传输任务
    xTaskCreate(udp_transmit_task, "udp_transmit", 4096, (void *)image_queue, 5, NULL);
    ESP_LOGI(TAG, "UDP transmit task started");

    // 初始化外设
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    
    // 初始化摄像头
    if (ESP_OK != init_camera()) {
        vTaskDelete(NULL);
        return;
    }

    // 初始化LCD
    esp_lcd_panel_handle_t panel_handle = audio_board_lcd_init(set, NULL);
    if (!panel_handle) {
        ESP_LOGE(TAG, "LCD initialization failed");
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        ESP_LOGI(TAG, "Taking picture...");
        // 获取摄像头帧
        camera_fb_t *pic = esp_camera_fb_get();
        if (!pic) {
            vTaskDelay(1); // 短暂让出CPU
            continue;
        }
        
        // use pic->buf to access the image
        ESP_LOGI(TAG, "Picture taken! The size was: %zu bytes, w:%d, h:%d", pic->len, pic->width, pic->height);
        example_lcd_rgb_draw(panel_handle, pic->buf);

        // 将图像发送到UDP传输队列
        if (xQueueSend(image_queue, &pic, 0) != pdTRUE) {
            ESP_LOGW(TAG, "UDP queue full, discarding frame");
            esp_camera_fb_return(pic);
        }

        AUDIO_MEM_SHOW(TAG);
        esp_camera_fb_return(pic);
        vTaskDelay(2);
    }
}

void my_lcd_camera_init(void)
{
    xTaskCreatePinnedToCore(
        my_lcd_camera_task,
        "cam_task",
        4096,
        NULL,
        4,
        NULL,
        1  // 在APP核心运行
    );
}